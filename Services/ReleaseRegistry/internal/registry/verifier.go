package registry

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"time"
)

type PackVerifier interface {
	Verify(ctx context.Context, artifact []byte, trustedPublicKey string) (PackMetadata, error)
}

type DeepPackValidator interface {
	Validate(ctx context.Context, artifact []byte, trustedPublicKey string) (PackMetadata, error)
}

type VerifiedPackVerifier struct {
	Deep DeepPackValidator
}

func (v VerifiedPackVerifier) Verify(
	ctx context.Context,
	artifact []byte,
	trustedPublicKey string,
) (PackMetadata, error) {
	metadata, err := InspectAndVerifyEnvelope(artifact)
	if err != nil {
		return PackMetadata{}, err
	}
	if metadata.PublisherPublicKey != trustedPublicKey {
		return PackMetadata{}, fmt.Errorf("%w: publisher key is not registered to the creator", ErrForbidden)
	}
	if v.Deep == nil {
		return PackMetadata{}, fmt.Errorf("%w: deep pack validator is not configured", ErrUnavailable)
	}
	deepMetadata, err := v.Deep.Validate(ctx, artifact, trustedPublicKey)
	if err != nil {
		return PackMetadata{}, err
	}
	if deepMetadata.ReleaseID != metadata.ReleaseID ||
		deepMetadata.ProjectID != metadata.ProjectID ||
		deepMetadata.SchemaVersion != metadata.SchemaVersion ||
		deepMetadata.ContentHash != metadata.ContentHash ||
		deepMetadata.PublisherPublicKey != metadata.PublisherPublicKey {
		return PackMetadata{}, fmt.Errorf("%w: validator metadata differs from the signed envelope", ErrInvalidInput)
	}
	return metadata, nil
}

type CommandValidator struct {
	Executable string
	Project    string
	TempRoot   string
	Timeout    time.Duration
	semaphore  chan struct{}
}

func NewCommandValidator(
	executable string,
	project string,
	tempRoot string,
	maxConcurrent int,
	timeout time.Duration,
) *CommandValidator {
	if maxConcurrent <= 0 {
		maxConcurrent = 1
	}
	if timeout <= 0 {
		timeout = 90 * time.Second
	}
	return &CommandValidator{
		Executable: executable,
		Project:    project,
		TempRoot:   tempRoot,
		Timeout:    timeout,
		semaphore:  make(chan struct{}, maxConcurrent),
	}
}

type commandValidationResult struct {
	Version       int    `json:"version"`
	ReleaseID     string `json:"releaseId"`
	ProjectID     string `json:"projectId"`
	SchemaVersion int32  `json:"schemaVersion"`
	ContentHash   string `json:"contentHash"`
	PublicKey     string `json:"publicKey"`
}

func (v CommandValidator) Validate(
	ctx context.Context,
	artifact []byte,
	trustedPublicKey string,
) (PackMetadata, error) {
	if v.Executable == "" || v.Project == "" {
		return PackMetadata{}, fmt.Errorf("%w: validator executable or project is missing", ErrUnavailable)
	}
	if v.semaphore == nil {
		return PackMetadata{}, fmt.Errorf("%w: validator concurrency gate is not configured", ErrUnavailable)
	}
	select {
	case v.semaphore <- struct{}{}:
		defer func() { <-v.semaphore }()
	case <-ctx.Done():
		return PackMetadata{}, fmt.Errorf("%w: validator wait canceled", ErrUnavailable)
	}
	validationContext, cancel := context.WithTimeout(ctx, v.Timeout)
	defer cancel()

	runDirectory, err := os.MkdirTemp(v.TempRoot, "akugc-registry-validate-")
	if err != nil {
		return PackMetadata{}, fmt.Errorf("%w: create validator directory: %v", ErrUnavailable, err)
	}
	defer os.RemoveAll(runDirectory)

	input := filepath.Join(runDirectory, "input.akugc")
	output := filepath.Join(runDirectory, "result.json")
	if err := os.WriteFile(input, artifact, 0o600); err != nil {
		return PackMetadata{}, fmt.Errorf("%w: write validator input: %v", ErrUnavailable, err)
	}

	command := exec.CommandContext(
		validationContext,
		v.Executable,
		v.Project,
		"-run=AkUGCPackTool",
		"-ValidateWork",
		"-Input="+input,
		"-Output="+output,
		"-TrustedKey="+trustedPublicKey,
		"-unattended",
		"-nop4",
		"-nosplash",
		"-nullrhi",
		"-stdout",
		"-abslog="+filepath.Join(runDirectory, "validator.log"),
	)
	var commandLog limitedBuffer
	commandLog.remaining = 64 * 1024
	command.Stdout = &commandLog
	command.Stderr = &commandLog
	if err := command.Run(); err != nil {
		if validationContext.Err() != nil {
			return PackMetadata{}, fmt.Errorf("%w: trusted pack validation timed out or was canceled", ErrUnavailable)
		}
		return PackMetadata{}, fmt.Errorf("%w: trusted pack validation rejected artifact", ErrInvalidInput)
	}

	raw, err := os.ReadFile(output)
	if err != nil {
		return PackMetadata{}, fmt.Errorf("%w: validator result is missing: %v", ErrUnavailable, err)
	}
	var result commandValidationResult
	decoder := json.NewDecoder(bytes.NewReader(raw))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(&result); err != nil || requireJSONEOF(decoder) != nil {
		return PackMetadata{}, fmt.Errorf("%w: validator returned invalid metadata", ErrUnavailable)
	}
	if result.Version != 1 {
		return PackMetadata{}, fmt.Errorf("%w: unsupported validator result version", ErrUnavailable)
	}
	releaseID, err := canonicalUUID(result.ReleaseID, true)
	if err != nil {
		return PackMetadata{}, fmt.Errorf("%w: validator returned invalid release ID", ErrUnavailable)
	}
	projectID, err := canonicalUUID(result.ProjectID, false)
	if err != nil {
		return PackMetadata{}, fmt.Errorf("%w: validator returned invalid project ID", ErrUnavailable)
	}
	if _, err := decodeLowerHex(result.ContentHash, 32); err != nil {
		return PackMetadata{}, fmt.Errorf("%w: validator returned invalid content hash", ErrUnavailable)
	}
	if _, err := decodeLowerHex(result.PublicKey, 32); err != nil {
		return PackMetadata{}, fmt.Errorf("%w: validator returned invalid public key", ErrUnavailable)
	}
	return PackMetadata{
		ReleaseID:          releaseID,
		ProjectID:          projectID,
		SchemaVersion:      result.SchemaVersion,
		ContentHash:        result.ContentHash,
		PublisherPublicKey: result.PublicKey,
	}, nil
}

type limitedBuffer struct {
	buffer    bytes.Buffer
	remaining int
}

func (b *limitedBuffer) Write(data []byte) (int, error) {
	originalLength := len(data)
	if len(data) > b.remaining {
		data = data[:b.remaining]
	}
	if len(data) > 0 {
		_, _ = b.buffer.Write(data)
		b.remaining -= len(data)
	}
	return originalLength, nil
}
