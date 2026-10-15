package registry

import (
	"context"
	"errors"
	"fmt"
	"os"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"akugcplatform/services/releaseregistry/migrations"
	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgxpool"
)

func TestPostgresImmutableLifecycleAndConcurrentRetry(t *testing.T) {
	store, pool := postgresTestStore(t)
	artifact, _, publicKey := signedArtifact(t, "91111111-2222-3333-4444-555555555555")
	metadata, err := InspectAndVerifyEnvelope(artifact)
	if err != nil {
		t.Fatal(err)
	}
	submission := Submission{
		OwnerSubject:   "creator-a",
		IdempotencyKey: "postgres-concurrent-91111111",
		Metadata:       metadata,
		Artifact:       artifact,
		SubmittedAt:    time.Date(2026, 10, 15, 10, 0, 0, 0, time.UTC),
	}

	var created atomic.Int32
	var wait sync.WaitGroup
	failures := make(chan error, 12)
	for index := 0; index < 12; index++ {
		wait.Add(1)
		go func() {
			defer wait.Done()
			release, wasCreated, err := store.Submit(context.Background(), submission)
			if err != nil {
				failures <- err
				return
			}
			if release.PublisherPublicKey != publicKey {
				failures <- fmt.Errorf("publisher key mismatch")
			}
			if wasCreated {
				created.Add(1)
			}
		}()
	}
	wait.Wait()
	close(failures)
	for err := range failures {
		t.Errorf("concurrent submit: %v", err)
	}
	if created.Load() != 1 {
		t.Fatalf("created count = %d", created.Load())
	}

	approved, changed, err := store.Transition(context.Background(), TransitionRequest{
		ReleaseID: metadata.ReleaseID,
		Actor:     "reviewer",
		Target:    StatusApproved,
		Reason:    "approved",
		At:        submission.SubmittedAt.Add(time.Minute),
	})
	if err != nil || !changed || approved.Status != StatusApproved {
		t.Fatalf("approve failed: changed=%v release=%+v err=%v", changed, approved, err)
	}
	published, changed, err := store.Transition(context.Background(), TransitionRequest{
		ReleaseID: metadata.ReleaseID,
		Actor:     "creator-a",
		Target:    StatusPublished,
		At:        submission.SubmittedAt.Add(2 * time.Minute),
	})
	if err != nil || !changed || published.Status != StatusPublished {
		t.Fatalf("publish failed: changed=%v release=%+v err=%v", changed, published, err)
	}

	_, err = pool.Exec(
		context.Background(),
		`UPDATE releases SET content_hash = $2 WHERE release_id = $1::uuid`,
		metadata.ReleaseID,
		fmt.Sprintf("%064d", 1),
	)
	if err == nil {
		t.Fatal("database allowed immutable content hash update")
	}
	stored, err := store.Get(context.Background(), metadata.ReleaseID)
	if err != nil || stored.ContentHash != metadata.ContentHash {
		t.Fatalf("immutable row changed: release=%+v err=%v", stored, err)
	}
}

func TestPostgresRejectsIDAndIdempotencyConflicts(t *testing.T) {
	store, _ := postgresTestStore(t)
	first, _, _ := signedArtifact(t, "a1111111-2222-3333-4444-555555555555")
	second, _, _ := signedArtifact(t, "b1111111-2222-3333-4444-555555555555")
	firstMetadata, _ := InspectAndVerifyEnvelope(first)
	secondMetadata, _ := InspectAndVerifyEnvelope(second)
	at := time.Date(2026, 10, 15, 11, 0, 0, 0, time.UTC)
	if _, _, err := store.Submit(context.Background(), Submission{
		OwnerSubject: "creator-a", IdempotencyKey: "postgres-conflict-key",
		Metadata: firstMetadata, Artifact: first, SubmittedAt: at,
	}); err != nil {
		t.Fatal(err)
	}
	if _, _, err := store.Submit(context.Background(), Submission{
		OwnerSubject: "creator-a", IdempotencyKey: "postgres-conflict-key",
		Metadata: secondMetadata, Artifact: second, SubmittedAt: at,
	}); !errors.Is(err, ErrConflict) {
		t.Fatalf("idempotency conflict was not rejected: %v", err)
	}

	collision := append(append([]byte(nil), first...), '\n')
	collisionMetadata, err := InspectAndVerifyEnvelope(collision)
	if err != nil {
		t.Fatal(err)
	}
	if _, _, err := store.Submit(context.Background(), Submission{
		OwnerSubject: "creator-a", IdempotencyKey: "postgres-release-collision",
		Metadata: collisionMetadata, Artifact: collision, SubmittedAt: at,
	}); !errors.Is(err, ErrConflict) {
		t.Fatalf("release ID conflict was not rejected: %v", err)
	}
}

func TestPostgresConcurrentReviewHasSingleWinner(t *testing.T) {
	store, _ := postgresTestStore(t)
	artifact, _, _ := signedArtifact(t, "c1111111-2222-3333-4444-555555555555")
	metadata, err := InspectAndVerifyEnvelope(artifact)
	if err != nil {
		t.Fatal(err)
	}
	at := time.Date(2026, 10, 15, 12, 0, 0, 0, time.UTC)
	if _, _, err := store.Submit(context.Background(), Submission{
		OwnerSubject: "creator-a", IdempotencyKey: "postgres-review-race",
		Metadata: metadata, Artifact: artifact, SubmittedAt: at,
	}); err != nil {
		t.Fatal(err)
	}

	results := make(chan error, 2)
	for _, target := range []Status{StatusApproved, StatusRejected} {
		target := target
		go func() {
			_, _, err := store.Transition(context.Background(), TransitionRequest{
				ReleaseID: metadata.ReleaseID,
				Actor:     "reviewer",
				Target:    target,
				Reason:    string(target),
				At:        at.Add(time.Minute),
			})
			results <- err
		}()
	}
	successes := 0
	conflicts := 0
	for range 2 {
		err := <-results
		switch {
		case err == nil:
			successes++
		case errors.Is(err, ErrConflict):
			conflicts++
		default:
			t.Fatalf("unexpected transition error: %v", err)
		}
	}
	if successes != 1 || conflicts != 1 {
		t.Fatalf("review race: successes=%d conflicts=%d", successes, conflicts)
	}
}

func postgresTestStore(t *testing.T) (*PostgresStore, *pgxpool.Pool) {
	t.Helper()
	databaseURL := os.Getenv("AKUGC_TEST_DATABASE_URL")
	if databaseURL == "" {
		t.Skip("AKUGC_TEST_DATABASE_URL is not configured")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	admin, err := pgxpool.New(ctx, databaseURL)
	if err != nil {
		t.Fatal(err)
	}
	schema := fmt.Sprintf("registry_test_%d", time.Now().UnixNano())
	identifier := pgx.Identifier{schema}.Sanitize()
	if _, err := admin.Exec(ctx, `CREATE SCHEMA `+identifier); err != nil {
		admin.Close()
		t.Fatal(err)
	}

	config, err := pgxpool.ParseConfig(databaseURL)
	if err != nil {
		t.Fatal(err)
	}
	config.ConnConfig.RuntimeParams["search_path"] = schema
	config.MaxConns = 16
	pool, err := pgxpool.NewWithConfig(ctx, config)
	if err != nil {
		t.Fatal(err)
	}
	if err := migrations.Apply(ctx, pool); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() {
		pool.Close()
		cleanupContext, cleanupCancel := context.WithTimeout(context.Background(), 15*time.Second)
		defer cleanupCancel()
		_, _ = admin.Exec(cleanupContext, `DROP SCHEMA `+identifier+` CASCADE`)
		admin.Close()
	})
	return NewPostgresStore(pool), pool
}
