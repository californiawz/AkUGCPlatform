package registry

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"runtime"
	"sync"
)

var (
	ErrArtifactNotFound = errors.New("artifact not found")
	ErrArtifactCorrupt  = errors.New("artifact corrupt")
)

// ArtifactStore stores immutable release artifacts by their lowercase SHA-256.
// Implementations must verify the digest on both Put and Get.
type ArtifactStore interface {
	Put(ctx context.Context, digest string, artifact []byte) error
	Get(ctx context.Context, digest string) ([]byte, error)
}

type MemoryArtifactStore struct {
	mutex     sync.RWMutex
	artifacts map[string][]byte
}

func NewMemoryArtifactStore() *MemoryArtifactStore {
	return &MemoryArtifactStore{artifacts: make(map[string][]byte)}
}

func (s *MemoryArtifactStore) Put(
	ctx context.Context,
	digest string,
	artifact []byte,
) error {
	if err := ctx.Err(); err != nil {
		return err
	}
	if err := verifyArtifact(digest, artifact); err != nil {
		return err
	}
	s.mutex.Lock()
	defer s.mutex.Unlock()
	if existing, exists := s.artifacts[digest]; exists {
		if err := verifyArtifact(digest, existing); err != nil {
			return fmt.Errorf("%w: existing in-memory object: %v", ErrArtifactCorrupt, err)
		}
		if !bytes.Equal(existing, artifact) {
			return fmt.Errorf("%w: SHA-256 collision for %s", ErrConflict, digest)
		}
		return nil
	}
	s.artifacts[digest] = append([]byte(nil), artifact...)
	return nil
}

func (s *MemoryArtifactStore) Get(
	ctx context.Context,
	digest string,
) ([]byte, error) {
	if err := validateArtifactDigest(digest); err != nil {
		return nil, err
	}
	if err := ctx.Err(); err != nil {
		return nil, err
	}
	s.mutex.RLock()
	artifact, exists := s.artifacts[digest]
	result := append([]byte(nil), artifact...)
	s.mutex.RUnlock()
	if !exists {
		return nil, ErrArtifactNotFound
	}
	if err := verifyArtifact(digest, result); err != nil {
		return nil, fmt.Errorf("%w: in-memory object %s: %v", ErrArtifactCorrupt, digest, err)
	}
	return result, nil
}

// LocalArtifactStore is a content-addressed store rooted at a private local
// directory. Object paths are derived solely from validated lowercase digests.
type LocalArtifactStore struct {
	root  string
	mutex sync.Mutex
}

func NewLocalArtifactStore(root string) (*LocalArtifactStore, error) {
	if root == "" {
		return nil, errors.New("artifact root is required")
	}
	absolute, err := filepath.Abs(root)
	if err != nil {
		return nil, fmt.Errorf("resolve artifact root: %w", err)
	}
	if err := os.MkdirAll(absolute, 0o700); err != nil {
		return nil, fmt.Errorf("create artifact root: %w", err)
	}
	if err := requireSafeDirectory(absolute); err != nil {
		return nil, fmt.Errorf("artifact root: %w", err)
	}
	return &LocalArtifactStore{root: filepath.Clean(absolute)}, nil
}

func (s *LocalArtifactStore) Put(
	ctx context.Context,
	digest string,
	artifact []byte,
) error {
	if err := verifyArtifact(digest, artifact); err != nil {
		return err
	}
	if err := ctx.Err(); err != nil {
		return err
	}

	s.mutex.Lock()
	defer s.mutex.Unlock()

	directory, path, err := s.safeObjectPath(digest, true)
	if err != nil {
		return err
	}
	if existing, err := readRegularFile(path); err == nil {
		if verifyErr := verifyArtifact(digest, existing); verifyErr != nil {
			return fmt.Errorf("%w: existing object %s: %v", ErrArtifactCorrupt, digest, verifyErr)
		}
		if !bytes.Equal(existing, artifact) {
			return fmt.Errorf("%w: SHA-256 collision for %s", ErrConflict, digest)
		}
		return nil
	} else if !errors.Is(err, os.ErrNotExist) {
		return err
	}

	temporary, err := os.CreateTemp(directory, "."+digest+".tmp-")
	if err != nil {
		return fmt.Errorf("%w: create artifact temporary file: %v", ErrUnavailable, err)
	}
	temporaryPath := temporary.Name()
	removeTemporary := true
	defer func() {
		_ = temporary.Close()
		if removeTemporary {
			_ = os.Remove(temporaryPath)
		}
	}()
	if err := temporary.Chmod(0o600); err != nil {
		return fmt.Errorf("%w: secure artifact temporary file: %v", ErrUnavailable, err)
	}
	if _, err := temporary.Write(artifact); err != nil {
		return fmt.Errorf("%w: write artifact temporary file: %v", ErrUnavailable, err)
	}
	if err := temporary.Sync(); err != nil {
		return fmt.Errorf("%w: sync artifact temporary file: %v", ErrUnavailable, err)
	}
	if err := temporary.Close(); err != nil {
		return fmt.Errorf("%w: close artifact temporary file: %v", ErrUnavailable, err)
	}
	written, err := readRegularFile(temporaryPath)
	if err != nil {
		return err
	}
	if err := verifyArtifact(digest, written); err != nil {
		return fmt.Errorf("%w: temporary object %s: %v", ErrArtifactCorrupt, digest, err)
	}

	// Link publishes the fully written object atomically and never replaces an
	// existing path. The temporary and final paths are on the same filesystem.
	if err := os.Link(temporaryPath, path); err != nil {
		if existing, readErr := readRegularFile(path); readErr == nil {
			if verifyErr := verifyArtifact(digest, existing); verifyErr != nil {
				return fmt.Errorf("%w: existing object %s: %v", ErrArtifactCorrupt, digest, verifyErr)
			}
			if !bytes.Equal(existing, artifact) {
				return fmt.Errorf("%w: SHA-256 collision for %s", ErrConflict, digest)
			}
			return nil
		}
		return fmt.Errorf("%w: publish artifact: %v", ErrUnavailable, err)
	}
	removeTemporary = false
	if err := os.Remove(temporaryPath); err != nil {
		return fmt.Errorf("%w: remove artifact temporary file: %v", ErrUnavailable, err)
	}
	if err := syncDirectory(directory); err != nil {
		return fmt.Errorf("%w: sync artifact directory: %v", ErrUnavailable, err)
	}
	return nil
}

func (s *LocalArtifactStore) Get(
	ctx context.Context,
	digest string,
) ([]byte, error) {
	if err := validateArtifactDigest(digest); err != nil {
		return nil, err
	}
	if err := ctx.Err(); err != nil {
		return nil, err
	}
	s.mutex.Lock()
	defer s.mutex.Unlock()
	_, path, err := s.safeObjectPath(digest, false)
	if err != nil {
		return nil, err
	}
	artifact, err := readRegularFile(path)
	if errors.Is(err, os.ErrNotExist) {
		return nil, ErrArtifactNotFound
	}
	if err != nil {
		return nil, err
	}
	if err := verifyArtifact(digest, artifact); err != nil {
		return nil, fmt.Errorf("%w: object %s: %v", ErrArtifactCorrupt, digest, err)
	}
	return artifact, nil
}

func (s *LocalArtifactStore) safeObjectPath(
	digest string,
	create bool,
) (string, string, error) {
	if err := validateArtifactDigest(digest); err != nil {
		return "", "", err
	}
	if err := requireSafeDirectory(s.root); err != nil {
		return "", "", fmt.Errorf("%w: unsafe artifact root: %v", ErrUnavailable, err)
	}
	namespace := filepath.Join(s.root, "sha256")
	prefix := filepath.Join(namespace, digest[:2])
	for _, directory := range []string{namespace, prefix} {
		if create {
			if err := os.Mkdir(directory, 0o700); err != nil && !errors.Is(err, os.ErrExist) {
				return "", "", fmt.Errorf("%w: create artifact directory: %v", ErrUnavailable, err)
			}
		}
		if err := requireSafeDirectory(directory); err != nil {
			if !create && errors.Is(err, os.ErrNotExist) {
				return prefix, filepath.Join(prefix, digest+".akugc"), nil
			}
			return "", "", fmt.Errorf("%w: unsafe artifact directory: %v", ErrUnavailable, err)
		}
	}
	path := filepath.Join(prefix, digest+".akugc")
	if filepath.Dir(path) != prefix {
		return "", "", fmt.Errorf("%w: invalid artifact path", ErrInvalidInput)
	}
	return prefix, path, nil
}

func validateArtifactDigest(digest string) error {
	if _, err := decodeLowerHex(digest, sha256.Size); err != nil {
		return fmt.Errorf("%w: artifact SHA-256 %v", ErrInvalidInput, err)
	}
	return nil
}

func verifyArtifact(digest string, artifact []byte) error {
	if err := validateArtifactDigest(digest); err != nil {
		return err
	}
	if len(artifact) < 1 || len(artifact) > MaxArtifactBytes {
		return fmt.Errorf(
			"%w: artifact size must be 1..%d bytes",
			ErrInvalidInput,
			MaxArtifactBytes,
		)
	}
	actual := sha256.Sum256(artifact)
	if hex.EncodeToString(actual[:]) != digest {
		return fmt.Errorf("%w: artifact SHA-256 mismatch", ErrInvalidInput)
	}
	return nil
}

func requireSafeDirectory(path string) error {
	info, err := os.Lstat(path)
	if err != nil {
		return err
	}
	if info.Mode()&os.ModeSymlink != 0 || !info.IsDir() {
		return fmt.Errorf("%s is not a regular directory", path)
	}
	return nil
}

func readRegularFile(path string) ([]byte, error) {
	info, err := os.Lstat(path)
	if err != nil {
		return nil, err
	}
	if info.Mode()&os.ModeSymlink != 0 || !info.Mode().IsRegular() {
		return nil, fmt.Errorf("%w: %s is not a regular file", ErrArtifactCorrupt, path)
	}
	if info.Size() < 1 || info.Size() > MaxArtifactBytes {
		return nil, fmt.Errorf("%w: object size is outside the allowed range", ErrArtifactCorrupt)
	}
	file, err := os.Open(path)
	if err != nil {
		return nil, fmt.Errorf("%w: open artifact: %v", ErrUnavailable, err)
	}
	defer file.Close()
	openedInfo, err := file.Stat()
	if err != nil {
		return nil, fmt.Errorf("%w: inspect open artifact: %v", ErrUnavailable, err)
	}
	currentInfo, err := os.Lstat(path)
	if err != nil {
		return nil, fmt.Errorf("%w: recheck artifact path: %v", ErrUnavailable, err)
	}
	if currentInfo.Mode()&os.ModeSymlink != 0 ||
		!openedInfo.Mode().IsRegular() ||
		!os.SameFile(openedInfo, currentInfo) {
		return nil, fmt.Errorf("%w: artifact path changed during access", ErrArtifactCorrupt)
	}
	if openedInfo.Size() < 1 || openedInfo.Size() > MaxArtifactBytes {
		return nil, fmt.Errorf("%w: object size is outside the allowed range", ErrArtifactCorrupt)
	}
	limited := io.LimitReader(file, MaxArtifactBytes+1)
	raw, err := io.ReadAll(limited)
	if err != nil {
		return nil, fmt.Errorf("%w: read artifact: %v", ErrUnavailable, err)
	}
	if len(raw) < 1 || len(raw) > MaxArtifactBytes {
		return nil, fmt.Errorf("%w: object size is outside the allowed range", ErrArtifactCorrupt)
	}
	return raw, nil
}

func syncDirectory(path string) error {
	directory, err := os.Open(path)
	if err != nil {
		return err
	}
	defer directory.Close()
	if err := directory.Sync(); err != nil {
		// Windows does not support syncing directory handles. The object itself
		// was already fsynced before publication.
		if runtime.GOOS == "windows" || errors.Is(err, os.ErrInvalid) {
			return nil
		}
		return err
	}
	return nil
}
