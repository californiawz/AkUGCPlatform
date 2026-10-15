package registry

import (
	"context"
	"encoding/json"
	"errors"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"
)

type packVerifierFunc func(context.Context, []byte, string) (PackMetadata, error)

func (function packVerifierFunc) Verify(
	ctx context.Context,
	artifact []byte,
	key string,
) (PackMetadata, error) {
	return function(ctx, artifact, key)
}

func TestBuildWorkerPromotesExactBytesAndUnlocksReview(t *testing.T) {
	fixture := newQueuedBuildFixture(t, "10111111-2222-3333-4444-555555555555")
	reviewer := Principal{Subject: "independent-reviewer", Roles: []Role{RoleReviewer}}
	if _, _, err := fixture.service.Review(
		context.Background(),
		reviewer,
		fixture.release.ReleaseID,
		StatusApproved,
		"",
	); !errors.Is(err, ErrConflict) {
		t.Fatalf("review before build error = %v", err)
	}

	worker := fixture.worker(acceptingVerifier{}, "success-worker")
	worked, err := worker.RunOnce(context.Background())
	if err != nil || !worked {
		t.Fatalf("worker result: worked=%v err=%v", worked, err)
	}
	build, err := fixture.store.GetBuild(context.Background(), fixture.release.ReleaseID)
	if err != nil ||
		build.Status != BuildStatusSucceeded ||
		build.Attempt != 1 ||
		build.OutputArtifactSHA256 != fixture.release.ArtifactSHA256 ||
		build.OutputArtifactSize != fixture.release.ArtifactSize {
		t.Fatalf("succeeded build mismatch: build=%+v err=%v", build, err)
	}
	raw, err := fixture.artifacts.Get(context.Background(), fixture.release.ArtifactSHA256)
	if err != nil || string(raw) != string(fixture.artifact) {
		t.Fatalf("worker changed artifact: size=%d err=%v", len(raw), err)
	}
	encoded, err := json.Marshal(build)
	if err != nil {
		t.Fatal(err)
	}
	if strings.Contains(string(encoded), "leaseOwner") ||
		strings.Contains(string(encoded), "leaseDeadline") {
		t.Fatalf("internal lease serialized: %s", encoded)
	}
	if _, changed, err := fixture.service.Review(
		context.Background(),
		reviewer,
		fixture.release.ReleaseID,
		StatusApproved,
		"",
	); err != nil || !changed {
		t.Fatalf("review after build: changed=%v err=%v", changed, err)
	}
}

func TestBuildWorkerNormalizesFailureAndExplicitRetry(t *testing.T) {
	fixture := newQueuedBuildFixture(t, "11111111-2222-3333-4444-555555555556")
	unavailable := packVerifierFunc(func(
		context.Context,
		[]byte,
		string,
	) (PackMetadata, error) {
		return PackMetadata{}, errors.Join(ErrUnavailable, errors.New("private validator details"))
	})
	if worked, err := fixture.worker(unavailable, "unavailable-worker").RunOnce(context.Background()); err != nil || !worked {
		t.Fatalf("unavailable worker: worked=%v err=%v", worked, err)
	}
	failed, err := fixture.service.GetBuild(
		context.Background(),
		fixture.creator,
		fixture.release.ReleaseID,
	)
	if err != nil ||
		failed.Status != BuildStatusFailed ||
		!failed.Retryable ||
		failed.ErrorCode != BuildErrorValidatorUnavailable {
		t.Fatalf("retryable failure mismatch: build=%+v err=%v", failed, err)
	}
	if strings.Contains(string(failed.ErrorCode), "private") {
		t.Fatalf("raw validator detail escaped: %+v", failed)
	}
	queued, changed, err := fixture.service.RetryBuild(
		context.Background(),
		fixture.creator,
		fixture.release.ReleaseID,
	)
	if err != nil || !changed ||
		queued.Status != BuildStatusQueued ||
		queued.Attempt != 0 ||
		queued.ErrorCode != "" {
		t.Fatalf("explicit retry mismatch: changed=%v build=%+v err=%v", changed, queued, err)
	}
	if worked, err := fixture.worker(acceptingVerifier{}, "recovery-worker").RunOnce(context.Background()); err != nil || !worked {
		t.Fatalf("recovery worker: worked=%v err=%v", worked, err)
	}
	succeeded, err := fixture.store.GetBuild(context.Background(), fixture.release.ReleaseID)
	if err != nil || succeeded.Status != BuildStatusSucceeded || succeeded.Attempt != 1 {
		t.Fatalf("recovered build mismatch: build=%+v err=%v", succeeded, err)
	}
}

func TestValidationFailureIsTerminal(t *testing.T) {
	fixture := newQueuedBuildFixture(t, "12111111-2222-3333-4444-555555555556")
	rejected := packVerifierFunc(func(
		context.Context,
		[]byte,
		string,
	) (PackMetadata, error) {
		return PackMetadata{}, errors.Join(ErrInvalidInput, errors.New("unsafe graph"))
	})
	if worked, err := fixture.worker(rejected, "validation-worker").RunOnce(context.Background()); err != nil || !worked {
		t.Fatalf("validation worker: worked=%v err=%v", worked, err)
	}
	build, err := fixture.store.GetBuild(context.Background(), fixture.release.ReleaseID)
	if err != nil ||
		build.Status != BuildStatusFailed ||
		build.Retryable ||
		build.ErrorCode != BuildErrorValidationRejected {
		t.Fatalf("terminal failure mismatch: build=%+v err=%v", build, err)
	}
	if _, _, err := fixture.service.RetryBuild(
		context.Background(),
		fixture.creator,
		fixture.release.ReleaseID,
	); !errors.Is(err, ErrConflict) {
		t.Fatalf("terminal retry error = %v", err)
	}
}

func TestMemoryBuildLeaseReclaimCapAndRetry(t *testing.T) {
	fixture := newQueuedBuildFixture(t, "13111111-2222-3333-4444-555555555556")
	at := time.Date(2026, 10, 15, 20, 0, 0, 0, time.UTC)
	for attempt, worker := range []string{"crashed-1", "crashed-2", "crashed-3"} {
		claimed, err := fixture.store.ClaimBuild(context.Background(), ClaimBuildRequest{
			Worker:        worker,
			At:            at.Add(time.Duration(attempt) * time.Minute),
			LeaseDuration: time.Minute,
		})
		if err != nil || claimed.Attempt != attempt+1 || claimed.Status != BuildStatusRunning {
			t.Fatalf("claim %d: build=%+v err=%v", attempt+1, claimed, err)
		}
	}
	if _, err := fixture.store.ClaimBuild(context.Background(), ClaimBuildRequest{
		Worker:        "reclaim-cap",
		At:            at.Add(3 * time.Minute),
		LeaseDuration: time.Minute,
	}); !errors.Is(err, ErrNoBuildAvailable) {
		t.Fatalf("cap claim error = %v", err)
	}
	failed, err := fixture.store.GetBuild(context.Background(), fixture.release.ReleaseID)
	if err != nil ||
		failed.Status != BuildStatusFailed ||
		!failed.Retryable ||
		failed.Attempt != MaxAutomaticBuildAttempts ||
		failed.ErrorCode != BuildErrorLeaseExpired {
		t.Fatalf("lease cap mismatch: build=%+v err=%v", failed, err)
	}
	retried, changed, err := fixture.store.RetryBuild(context.Background(), RetryBuildRequest{
		ReleaseID: fixture.release.ReleaseID,
		Actor:     fixture.creator.Subject,
		At:        at.Add(4 * time.Minute),
	})
	if err != nil || !changed || retried.Attempt != 0 || retried.Status != BuildStatusQueued {
		t.Fatalf("lease retry mismatch: changed=%v build=%+v err=%v", changed, retried, err)
	}
}

func TestConcurrentBuildClaimAndCompletionAreDeterministic(t *testing.T) {
	fixture := newQueuedBuildFixture(t, "14111111-2222-3333-4444-555555555556")
	at := time.Date(2026, 10, 15, 21, 0, 0, 0, time.UTC)
	var claims atomic.Int32
	var wait sync.WaitGroup
	failures := make(chan error, 64)
	for range 32 {
		wait.Add(1)
		go func() {
			defer wait.Done()
			_, err := fixture.store.ClaimBuild(context.Background(), ClaimBuildRequest{
				Worker:        "one-worker",
				At:            at,
				LeaseDuration: time.Minute,
			})
			switch {
			case err == nil:
				claims.Add(1)
			case errors.Is(err, ErrNoBuildAvailable):
			default:
				failures <- err
			}
		}()
	}
	wait.Wait()
	if claims.Load() != 1 {
		t.Fatalf("claim winners = %d", claims.Load())
	}
	var completions atomic.Int32
	for range 32 {
		wait.Add(1)
		go func() {
			defer wait.Done()
			_, changed, err := fixture.store.CompleteBuild(
				context.Background(),
				CompleteBuildRequest{
					ReleaseID:            fixture.release.ReleaseID,
					Worker:               "one-worker",
					Succeeded:            true,
					OutputArtifactSHA256: fixture.release.ArtifactSHA256,
					OutputArtifactSize:   fixture.release.ArtifactSize,
					At:                   at.Add(time.Second),
				},
			)
			if err != nil {
				failures <- err
				return
			}
			if changed {
				completions.Add(1)
			}
		}()
	}
	wait.Wait()
	close(failures)
	for err := range failures {
		t.Errorf("concurrent build operation: %v", err)
	}
	if completions.Load() != 1 {
		t.Fatalf("completion changes = %d", completions.Load())
	}
}

func TestLateBuildCompletionCannotOverwriteReclaimedLease(t *testing.T) {
	fixture := newQueuedBuildFixture(t, "15111111-2222-3333-4444-555555555556")
	at := time.Date(2026, 10, 15, 22, 0, 0, 0, time.UTC)
	first, err := fixture.store.ClaimBuild(context.Background(), ClaimBuildRequest{
		Worker:        "worker-a",
		At:            at,
		LeaseDuration: time.Minute,
	})
	if err != nil || first.Attempt != 1 || first.leaseOwner != "worker-a" {
		t.Fatalf("first claim: build=%+v err=%v", first, err)
	}
	reclaimed, err := fixture.store.ClaimBuild(context.Background(), ClaimBuildRequest{
		Worker:        "worker-b",
		At:            at.Add(time.Minute),
		LeaseDuration: time.Minute,
	})
	if err != nil || reclaimed.Attempt != 2 || reclaimed.leaseOwner != "worker-b" {
		t.Fatalf("reclaimed claim: build=%+v err=%v", reclaimed, err)
	}
	if _, changed, err := fixture.store.CompleteBuild(
		context.Background(),
		CompleteBuildRequest{
			ReleaseID:            fixture.release.ReleaseID,
			Worker:               "worker-a",
			Succeeded:            true,
			OutputArtifactSHA256: fixture.release.ArtifactSHA256,
			OutputArtifactSize:   fixture.release.ArtifactSize,
			At:                   at.Add(time.Minute + time.Second),
		},
	); err == nil || !errors.Is(err, ErrConflict) || changed {
		t.Fatalf("late completion: changed=%v err=%v", changed, err)
	}
	current, err := fixture.store.GetBuild(context.Background(), fixture.release.ReleaseID)
	if err != nil ||
		current.Status != BuildStatusRunning ||
		current.Attempt != 2 ||
		current.leaseOwner != "worker-b" {
		t.Fatalf("reclaimed build was overwritten: build=%+v err=%v", current, err)
	}
	completed, changed, err := fixture.store.CompleteBuild(
		context.Background(),
		CompleteBuildRequest{
			ReleaseID:            fixture.release.ReleaseID,
			Worker:               "worker-b",
			Succeeded:            true,
			OutputArtifactSHA256: fixture.release.ArtifactSHA256,
			OutputArtifactSize:   fixture.release.ArtifactSize,
			At:                   at.Add(time.Minute + 2*time.Second),
		},
	)
	if err != nil || !changed || completed.Status != BuildStatusSucceeded {
		t.Fatalf("current lease completion: changed=%v build=%+v err=%v", changed, completed, err)
	}
}

type queuedBuildFixture struct {
	store     *MemoryStore
	artifacts *MemoryArtifactStore
	service   *Service
	creator   Principal
	artifact  []byte
	release   Release
	now       time.Time
}

func newQueuedBuildFixture(t *testing.T, releaseID string) *queuedBuildFixture {
	t.Helper()
	artifact, _, publicKey := signedArtifact(t, releaseID)
	store := NewMemoryStore()
	artifacts := NewMemoryArtifactStore()
	now := time.Date(2026, 10, 15, 19, 0, 0, 0, time.UTC)
	service := NewService(store, artifacts, func() time.Time {
		now = now.Add(time.Second)
		return now
	})
	creator := Principal{
		Subject:       "creator-a",
		Roles:         []Role{RoleCreator, RolePublisher},
		PublisherKeys: []string{publicKey},
	}
	createServiceTestProject(t, service, creator)
	release, build, created, err := service.Submit(
		context.Background(),
		creator,
		"queued-build-"+strings.ReplaceAll(releaseID[:8], "-", ""),
		artifact,
	)
	if err != nil || !created || build.Status != BuildStatusQueued {
		t.Fatalf("queue build: created=%v release=%+v build=%+v err=%v", created, release, build, err)
	}
	return &queuedBuildFixture{
		store:     store,
		artifacts: artifacts,
		service:   service,
		creator:   creator,
		artifact:  artifact,
		release:   release,
		now:       now,
	}
}

func (f *queuedBuildFixture) worker(verifier PackVerifier, name string) *BuildWorker {
	worker := NewBuildWorker(f.store, f.artifacts, verifier, name)
	worker.Now = func() time.Time {
		f.now = f.now.Add(time.Second)
		return f.now
	}
	return worker
}
