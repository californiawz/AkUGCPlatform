package registry

import (
	"context"
	"errors"
	"fmt"
	"time"
)

type BuildWorker struct {
	Store         Store
	Artifacts     ArtifactStore
	Verifier      PackVerifier
	Worker        string
	LeaseDuration time.Duration
	PollInterval  time.Duration
	Now           func() time.Time
}

func NewBuildWorker(
	store Store,
	artifacts ArtifactStore,
	verifier PackVerifier,
	worker string,
) *BuildWorker {
	return &BuildWorker{
		Store:         store,
		Artifacts:     artifacts,
		Verifier:      verifier,
		Worker:        worker,
		LeaseDuration: 2 * time.Minute,
		PollInterval:  500 * time.Millisecond,
		Now:           func() time.Time { return time.Now().UTC() },
	}
}

func (w *BuildWorker) Run(ctx context.Context) error {
	if err := w.validate(); err != nil {
		return err
	}
	for {
		worked, err := w.RunOnce(ctx)
		if err != nil {
			if ctx.Err() != nil {
				return nil
			}
			return err
		}
		if worked {
			continue
		}
		timer := time.NewTimer(w.PollInterval)
		select {
		case <-ctx.Done():
			if !timer.Stop() {
				<-timer.C
			}
			return nil
		case <-timer.C:
		}
	}
}

func (w *BuildWorker) RunOnce(ctx context.Context) (bool, error) {
	if err := w.validate(); err != nil {
		return false, err
	}
	build, err := w.Store.ClaimBuild(ctx, ClaimBuildRequest{
		Worker:        w.Worker,
		At:            w.now(),
		LeaseDuration: w.LeaseDuration,
	})
	if errors.Is(err, ErrNoBuildAvailable) {
		return false, nil
	}
	if err != nil {
		return false, err
	}
	if err := w.process(ctx, build); err != nil {
		return true, err
	}
	return true, nil
}

func (w *BuildWorker) process(ctx context.Context, build BuildJob) error {
	release, err := w.Store.Get(ctx, build.ReleaseID)
	if err != nil {
		return err
	}
	artifact, err := w.Artifacts.Get(ctx, build.InputArtifactSHA256)
	if err != nil {
		code := BuildErrorArtifactUnavailable
		if errors.Is(err, ErrArtifactCorrupt) {
			code = BuildErrorArtifactCorrupt
		}
		return w.completeFailure(ctx, build.ReleaseID, code)
	}
	if int64(len(artifact)) != release.ArtifactSize {
		return w.completeFailure(ctx, build.ReleaseID, BuildErrorArtifactCorrupt)
	}
	if err := verifyArtifact(release.ArtifactSHA256, artifact); err != nil {
		return w.completeFailure(ctx, build.ReleaseID, BuildErrorArtifactCorrupt)
	}
	metadata, err := w.Verifier.Verify(
		ctx,
		artifact,
		release.PublisherPublicKey,
	)
	if err != nil {
		code := BuildErrorValidatorUnavailable
		if errors.Is(err, ErrInvalidInput) || errors.Is(err, ErrForbidden) {
			code = BuildErrorValidationRejected
		}
		return w.completeFailure(ctx, build.ReleaseID, code)
	}
	if !metadataMatchesRelease(metadata, release) {
		return w.completeFailure(ctx, build.ReleaseID, BuildErrorValidationRejected)
	}
	_, _, err = w.Store.CompleteBuild(ctx, CompleteBuildRequest{
		ReleaseID:            build.ReleaseID,
		Worker:               w.Worker,
		Succeeded:            true,
		OutputArtifactSHA256: release.ArtifactSHA256,
		OutputArtifactSize:   release.ArtifactSize,
		At:                   w.now(),
	})
	return err
}

func (w *BuildWorker) completeFailure(
	ctx context.Context,
	releaseID string,
	code BuildErrorCode,
) error {
	_, _, err := w.Store.CompleteBuild(ctx, CompleteBuildRequest{
		ReleaseID: releaseID,
		Worker:    w.Worker,
		ErrorCode: code,
		At:        w.now(),
	})
	return err
}

func (w *BuildWorker) validate() error {
	switch {
	case w.Store == nil:
		return fmt.Errorf("%w: build store is not configured", ErrUnavailable)
	case w.Artifacts == nil:
		return fmt.Errorf("%w: artifact store is not configured", ErrUnavailable)
	case w.Verifier == nil:
		return fmt.Errorf("%w: build verifier is not configured", ErrUnavailable)
	case w.Worker == "":
		return fmt.Errorf("%w: build worker identity is empty", ErrInvalidInput)
	case w.LeaseDuration <= 0:
		return fmt.Errorf("%w: build lease duration must be positive", ErrInvalidInput)
	case w.PollInterval <= 0:
		return fmt.Errorf("%w: build poll interval must be positive", ErrInvalidInput)
	case w.Now == nil:
		return fmt.Errorf("%w: build clock is not configured", ErrUnavailable)
	default:
		return nil
	}
}

func (w *BuildWorker) now() time.Time {
	return w.Now().UTC()
}

func metadataMatchesRelease(metadata PackMetadata, release Release) bool {
	return metadata.ReleaseID == release.ReleaseID &&
		metadata.ProjectID == release.ProjectID &&
		metadata.SchemaVersion == release.SchemaVersion &&
		metadata.ContentHash == release.ContentHash &&
		metadata.PublisherPublicKey == release.PublisherPublicKey &&
		metadata.ArtifactSHA256 == release.ArtifactSHA256 &&
		metadata.ArtifactSize == release.ArtifactSize
}
