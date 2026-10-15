package registry

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"os"
	"path/filepath"
	"sync"
	"testing"
)

func TestMemoryArtifactStoreVerifiesAndClones(t *testing.T) {
	store := NewMemoryArtifactStore()
	artifact := []byte("signed-artifact")
	digest := artifactDigest(artifact)
	if err := store.Put(context.Background(), digest, artifact); err != nil {
		t.Fatal(err)
	}
	artifact[0] = 'X'
	first, err := store.Get(context.Background(), digest)
	if err != nil || string(first) != "signed-artifact" {
		t.Fatalf("stored artifact changed: artifact=%q err=%v", first, err)
	}
	first[0] = 'Y'
	second, err := store.Get(context.Background(), digest)
	if err != nil || string(second) != "signed-artifact" {
		t.Fatalf("returned artifact was not cloned: artifact=%q err=%v", second, err)
	}

	store.mutex.Lock()
	store.artifacts[digest][0] = 'Z'
	store.mutex.Unlock()
	if _, err := store.Get(context.Background(), digest); !errors.Is(err, ErrArtifactCorrupt) {
		t.Fatalf("corrupt memory object error = %v", err)
	}
}

func TestArtifactStoreRejectsInvalidInputs(t *testing.T) {
	store := NewMemoryArtifactStore()
	valid := []byte("artifact")
	for name, testCase := range map[string]struct {
		digest   string
		artifact []byte
	}{
		"uppercase digest": {
			digest:   "AA" + artifactDigest(valid)[2:],
			artifact: valid,
		},
		"digest mismatch": {
			digest:   artifactDigest([]byte("different")),
			artifact: valid,
		},
		"empty": {
			digest:   artifactDigest(nil),
			artifact: nil,
		},
		"oversized": {
			digest:   artifactDigest(make([]byte, MaxArtifactBytes+1)),
			artifact: make([]byte, MaxArtifactBytes+1),
		},
	} {
		t.Run(name, func(t *testing.T) {
			if err := store.Put(context.Background(), testCase.digest, testCase.artifact); !errors.Is(err, ErrInvalidInput) {
				t.Fatalf("error = %v", err)
			}
		})
	}
	if _, err := store.Get(context.Background(), artifactDigest(valid)); !errors.Is(err, ErrArtifactNotFound) {
		t.Fatalf("missing object error = %v", err)
	}
}

func TestLocalArtifactStoreAtomicCASAndCorruptionDetection(t *testing.T) {
	root := t.TempDir()
	store, err := NewLocalArtifactStore(root)
	if err != nil {
		t.Fatal(err)
	}
	artifact := []byte("immutable signed bytes")
	digest := artifactDigest(artifact)

	var wait sync.WaitGroup
	failures := make(chan error, 16)
	for range 16 {
		wait.Add(1)
		go func() {
			defer wait.Done()
			if err := store.Put(context.Background(), digest, artifact); err != nil {
				failures <- err
			}
		}()
	}
	wait.Wait()
	close(failures)
	for err := range failures {
		t.Errorf("concurrent Put: %v", err)
	}
	path := filepath.Join(root, "sha256", digest[:2], digest+".akugc")
	stored, err := os.ReadFile(path)
	if err != nil || !bytes.Equal(stored, artifact) {
		t.Fatalf("CAS object mismatch: artifact=%q err=%v", stored, err)
	}
	loaded, err := store.Get(context.Background(), digest)
	if err != nil || !bytes.Equal(loaded, artifact) {
		t.Fatalf("Get mismatch: artifact=%q err=%v", loaded, err)
	}

	if err := os.WriteFile(path, []byte("tampered"), 0o600); err != nil {
		t.Fatal(err)
	}
	if _, err := store.Get(context.Background(), digest); !errors.Is(err, ErrArtifactCorrupt) {
		t.Fatalf("tampered Get error = %v", err)
	}
	if err := store.Put(context.Background(), digest, artifact); !errors.Is(err, ErrArtifactCorrupt) {
		t.Fatalf("tampered collision Put error = %v", err)
	}
}

func TestLocalArtifactStoreRejectsSymlinkedObjectPath(t *testing.T) {
	root := t.TempDir()
	store, err := NewLocalArtifactStore(root)
	if err != nil {
		t.Fatal(err)
	}
	artifact := []byte("artifact through unsafe path")
	digest := artifactDigest(artifact)
	namespace := filepath.Join(root, "sha256")
	if err := os.Mkdir(namespace, 0o700); err != nil {
		t.Fatal(err)
	}
	external := t.TempDir()
	if err := os.Symlink(external, filepath.Join(namespace, digest[:2])); err != nil {
		t.Skipf("symlink creation is unavailable: %v", err)
	}
	if err := store.Put(context.Background(), digest, artifact); !errors.Is(err, ErrUnavailable) {
		t.Fatalf("unsafe path error = %v", err)
	}
}

func artifactDigest(artifact []byte) string {
	digest := sha256.Sum256(artifact)
	return hex.EncodeToString(digest[:])
}
