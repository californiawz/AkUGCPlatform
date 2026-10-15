package registry

import (
	"bytes"
	"context"
	"errors"
	"sync"
	"sync/atomic"
	"testing"
	"time"
)

type acceptingVerifier struct{}

func (acceptingVerifier) Verify(
	_ context.Context,
	artifact []byte,
	trustedKey string,
) (PackMetadata, error) {
	metadata, err := InspectAndVerifyEnvelope(artifact)
	if err != nil {
		return PackMetadata{}, err
	}
	if metadata.PublisherPublicKey != trustedKey {
		return PackMetadata{}, ErrForbidden
	}
	return metadata, nil
}

type countingVerifier struct {
	calls atomic.Int32
}

func (v *countingVerifier) Verify(
	ctx context.Context,
	artifact []byte,
	trustedKey string,
) (PackMetadata, error) {
	v.calls.Add(1)
	time.Sleep(25 * time.Millisecond)
	return acceptingVerifier{}.Verify(ctx, artifact, trustedKey)
}

func TestReleaseLifecyclePermissionsAndImmutability(t *testing.T) {
	artifact, _, publicKey := signedArtifact(t, "31111111-2222-3333-4444-555555555555")
	now := time.Date(2026, 10, 15, 8, 0, 0, 0, time.UTC)
	service := NewService(NewMemoryStore(), acceptingVerifier{}, func() time.Time {
		now = now.Add(time.Second)
		return now
	})
	creator := Principal{
		Subject:       "creator-a",
		Roles:         []Role{RoleCreator, RolePublisher},
		PublisherKeys: []string{publicKey},
	}
	reviewer := Principal{Subject: "reviewer", Roles: []Role{RoleReviewer}}
	player := Principal{Subject: "player-b", Roles: []Role{RolePlayer}}

	release, created, err := service.Submit(
		context.Background(),
		creator,
		"submit-31111111-v1",
		artifact,
	)
	if err != nil || !created || release.Status != StatusSubmitted {
		t.Fatalf("submit failed: created=%v release=%+v err=%v", created, release, err)
	}
	retried, created, err := service.Submit(
		context.Background(),
		creator,
		"submit-31111111-v1",
		artifact,
	)
	if err != nil || created || retried.ReleaseID != release.ReleaseID {
		t.Fatalf("idempotent retry failed: created=%v release=%+v err=%v", created, retried, err)
	}

	if _, _, err := service.Publish(context.Background(), creator, release.ReleaseID); !errors.Is(err, ErrConflict) {
		t.Fatalf("unapproved publish should conflict: %v", err)
	}
	selfReviewer := creator
	selfReviewer.Roles = append(selfReviewer.Roles, RoleReviewer)
	if _, _, err := service.Review(
		context.Background(),
		selfReviewer,
		release.ReleaseID,
		StatusApproved,
		"",
	); !errors.Is(err, ErrForbidden) {
		t.Fatalf("self review should be forbidden: %v", err)
	}
	approved, changed, err := service.Review(
		context.Background(),
		reviewer,
		release.ReleaseID,
		StatusApproved,
		"fixture approved",
	)
	if err != nil || !changed || approved.Status != StatusApproved {
		t.Fatalf("review failed: changed=%v release=%+v err=%v", changed, approved, err)
	}
	if _, _, err := service.Review(
		context.Background(),
		reviewer,
		release.ReleaseID,
		StatusRejected,
		"late rejection",
	); !errors.Is(err, ErrConflict) {
		t.Fatalf("conflicting review should fail: %v", err)
	}
	published, changed, err := service.Publish(context.Background(), creator, release.ReleaseID)
	if err != nil || !changed || published.Status != StatusPublished {
		t.Fatalf("publish failed: changed=%v release=%+v err=%v", changed, published, err)
	}
	downloaded, err := service.Download(context.Background(), player, release.ReleaseID)
	if err != nil || !bytes.Equal(downloaded.Artifact, artifact) {
		t.Fatalf("published download failed: release=%+v err=%v", downloaded, err)
	}

	collision := append(append([]byte(nil), artifact...), '\n')
	if _, _, err := service.Submit(
		context.Background(),
		creator,
		"submit-collision-v1",
		collision,
	); !errors.Is(err, ErrConflict) {
		t.Fatalf("release ID collision should fail: %v", err)
	}
	revoked, changed, err := service.Revoke(
		context.Background(),
		creator,
		release.ReleaseID,
		"security withdrawal",
	)
	if err != nil || !changed || revoked.Status != StatusRevoked {
		t.Fatalf("revoke failed: changed=%v release=%+v err=%v", changed, revoked, err)
	}
	if _, err := service.Download(context.Background(), player, release.ReleaseID); !errors.Is(err, ErrNotFound) {
		t.Fatalf("revoked download should be hidden: %v", err)
	}
}

func TestConcurrentSubmissionRetryCreatesOneRelease(t *testing.T) {
	artifact, _, publicKey := signedArtifact(t, "41111111-2222-3333-4444-555555555555")
	verifier := &countingVerifier{}
	service := NewService(NewMemoryStore(), verifier, func() time.Time {
		return time.Date(2026, 10, 15, 9, 0, 0, 0, time.UTC)
	})
	creator := Principal{
		Subject:       "creator-a",
		Roles:         []Role{RoleCreator},
		PublisherKeys: []string{publicKey},
	}

	var createdCount atomic.Int32
	var wait sync.WaitGroup
	errorsFound := make(chan error, 32)
	for index := 0; index < 32; index++ {
		wait.Add(1)
		go func() {
			defer wait.Done()
			_, created, err := service.Submit(
				context.Background(),
				creator,
				"concurrent-submit-41111111",
				artifact,
			)
			if err != nil {
				errorsFound <- err
				return
			}
			if created {
				createdCount.Add(1)
			}
		}()
	}
	wait.Wait()
	close(errorsFound)
	for err := range errorsFound {
		t.Errorf("concurrent submit failed: %v", err)
	}
	if createdCount.Load() != 1 {
		t.Fatalf("expected exactly one creation, got %d", createdCount.Load())
	}
	if verifier.calls.Load() != 1 {
		t.Fatalf("expected one shared deep validation, got %d", verifier.calls.Load())
	}
}

func TestIdempotencyKeyCannotBindDifferentArtifact(t *testing.T) {
	first, _, publicKey := signedArtifact(t, "51111111-2222-3333-4444-555555555555")
	second, _, _ := signedArtifact(t, "61111111-2222-3333-4444-555555555555")
	service := NewService(NewMemoryStore(), acceptingVerifier{}, nil)
	creator := Principal{
		Subject:       "creator-a",
		Roles:         []Role{RoleCreator},
		PublisherKeys: []string{publicKey},
	}
	if _, _, err := service.Submit(
		context.Background(), creator, "same-key-different-v1", first,
	); err != nil {
		t.Fatal(err)
	}
	if _, _, err := service.Submit(
		context.Background(), creator, "same-key-different-v1", second,
	); !errors.Is(err, ErrConflict) {
		t.Fatalf("different artifact reused idempotency key: %v", err)
	}
}
