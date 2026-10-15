package registry

import (
	"bytes"
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
	project := createPostgresTestProject(
		t,
		store,
		metadata.ProjectID,
		time.Date(2026, 10, 15, 9, 59, 0, 0, time.UTC),
	)
	submission := Submission{
		SubmitterSubject: "creator-a",
		IdempotencyKey:   "postgres-concurrent-91111111",
		Metadata:         metadata,
		Artifact:         artifact,
		SubmittedAt:      time.Date(2026, 10, 15, 10, 0, 0, 0, time.UTC),
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
			if release.OwnerSubject != project.OwnerSubject ||
				release.SubmittedBySubject != "creator-a" {
				failures <- fmt.Errorf("release attribution mismatch")
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
	latest, err := store.GetLatestPublishedRelease(context.Background(), metadata.ProjectID)
	if err != nil || latest.ReleaseID != metadata.ReleaseID ||
		!bytes.Equal(latest.Artifact, artifact) {
		t.Fatalf("latest published release mismatch: release=%+v err=%v", latest, err)
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
	createPostgresTestProject(t, store, firstMetadata.ProjectID, at.Add(-time.Minute))
	if _, _, err := store.Submit(context.Background(), Submission{
		SubmitterSubject: "creator-a", IdempotencyKey: "postgres-conflict-key",
		Metadata: firstMetadata, Artifact: first, SubmittedAt: at,
	}); err != nil {
		t.Fatal(err)
	}
	if _, _, err := store.Submit(context.Background(), Submission{
		SubmitterSubject: "creator-a", IdempotencyKey: "postgres-conflict-key",
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
		SubmitterSubject: "creator-a", IdempotencyKey: "postgres-release-collision",
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
	createPostgresTestProject(t, store, metadata.ProjectID, at.Add(-time.Minute))
	if _, _, err := store.Submit(context.Background(), Submission{
		SubmitterSubject: "creator-a", IdempotencyKey: "postgres-review-race",
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

func TestPostgresProjectMembershipAndVersionPaging(t *testing.T) {
	store, pool := postgresTestStore(t)
	at := time.Date(2026, 10, 15, 13, 0, 0, 0, time.UTC)
	creation := ProjectCreation{
		ProjectID:      "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee",
		DisplayName:    "PostgreSQL collaboration",
		OwnerSubject:   "creator-a",
		IdempotencyKey: "postgres-project-concurrent-v1",
		CreatedAt:      at,
	}
	var created atomic.Int32
	failures := make(chan error, 12)
	var wait sync.WaitGroup
	for index := 0; index < 12; index++ {
		wait.Add(1)
		go func() {
			defer wait.Done()
			project, wasCreated, err := store.CreateProject(context.Background(), creation)
			if err != nil {
				failures <- err
				return
			}
			if project.OwnerSubject != creation.OwnerSubject {
				failures <- fmt.Errorf("project owner mismatch")
			}
			if wasCreated {
				created.Add(1)
			}
		}()
	}
	wait.Wait()
	close(failures)
	for err := range failures {
		t.Errorf("concurrent project creation: %v", err)
	}
	if created.Load() != 1 {
		t.Fatalf("project created count = %d", created.Load())
	}
	storedProject, err := store.GetProject(context.Background(), creation.ProjectID)
	if err != nil || storedProject.Visibility != ProjectVisibilityPrivate {
		t.Fatalf("new PostgreSQL project visibility: project=%+v err=%v", storedProject, err)
	}

	member, project, changed, err := store.SetProjectMember(
		context.Background(),
		ProjectMemberChange{
			ProjectID: creation.ProjectID,
			Actor:     creation.OwnerSubject,
			Subject:   "creator-editor",
			Role:      ProjectRoleEditor,
			ChangedAt: at.Add(time.Minute),
		},
	)
	if err != nil || !changed || member.Role != ProjectRoleEditor || project.Revision != 2 {
		t.Fatalf("set editor failed: member=%+v project=%+v changed=%v err=%v", member, project, changed, err)
	}
	artifact, _, _ := signedArtifact(t, "d2111111-2222-3333-4444-555555555555")
	metadata, err := InspectAndVerifyEnvelope(artifact)
	if err != nil {
		t.Fatal(err)
	}
	release, wasCreated, err := store.Submit(context.Background(), Submission{
		SubmitterSubject: "creator-editor",
		IdempotencyKey:   "postgres-editor-submit-v1",
		Metadata:         metadata,
		Artifact:         artifact,
		SubmittedAt:      at.Add(2 * time.Minute),
	})
	if err != nil || !wasCreated ||
		release.OwnerSubject != creation.OwnerSubject ||
		release.SubmittedBySubject != "creator-editor" {
		t.Fatalf("editor submit failed: created=%v release=%+v err=%v", wasCreated, release, err)
	}
	page, err := store.ListProjectVersions(
		context.Background(),
		creation.ProjectID,
		"",
		1,
	)
	if err != nil || len(page.Releases) != 1 || page.Releases[0].Artifact != nil {
		t.Fatalf("version page mismatch: page=%+v err=%v", page, err)
	}

	project, changed, err = store.RemoveProjectMember(
		context.Background(),
		ProjectMemberRemoval{
			ProjectID: creation.ProjectID,
			Actor:     creation.OwnerSubject,
			Subject:   "creator-editor",
			ChangedAt: at.Add(3 * time.Minute),
		},
	)
	if err != nil || !changed || project.Revision != 3 {
		t.Fatalf("remove editor failed: project=%+v changed=%v err=%v", project, changed, err)
	}
	var visibilityChanges atomic.Int32
	failures = make(chan error, 12)
	for index := 0; index < 12; index++ {
		wait.Add(1)
		go func() {
			defer wait.Done()
			visible, wasChanged, err := store.SetProjectVisibility(
				context.Background(),
				ProjectVisibilityChange{
					ProjectID:  creation.ProjectID,
					Actor:      creation.OwnerSubject,
					Visibility: ProjectVisibilityPublic,
					ChangedAt:  at.Add(4 * time.Minute),
				},
			)
			if err != nil {
				failures <- err
				return
			}
			if visible.Visibility != ProjectVisibilityPublic {
				failures <- fmt.Errorf("visibility mismatch")
			}
			if wasChanged {
				visibilityChanges.Add(1)
			}
		}()
	}
	wait.Wait()
	close(failures)
	for err := range failures {
		t.Errorf("concurrent visibility change: %v", err)
	}
	if visibilityChanges.Load() != 1 {
		t.Fatalf("visibility changed count = %d", visibilityChanges.Load())
	}
	project, err = store.GetProject(context.Background(), creation.ProjectID)
	if err != nil || project.Revision != 4 ||
		project.Visibility != ProjectVisibilityPublic {
		t.Fatalf("stored visibility mismatch: project=%+v err=%v", project, err)
	}
	var visibilityAuditCount int
	if err := pool.QueryRow(
		context.Background(),
		`SELECT count(*) FROM project_visibility_events
		 WHERE project_id = $1::uuid`,
		creation.ProjectID,
	).Scan(&visibilityAuditCount); err != nil || visibilityAuditCount != 1 {
		t.Fatalf("visibility audit count=%d err=%v", visibilityAuditCount, err)
	}
	second, _, _ := signedArtifact(t, "e2111111-2222-3333-4444-555555555555")
	secondMetadata, _ := InspectAndVerifyEnvelope(second)
	if _, _, err := store.Submit(context.Background(), Submission{
		SubmitterSubject: "creator-editor",
		IdempotencyKey:   "postgres-removed-editor-v1",
		Metadata:         secondMetadata,
		Artifact:         second,
		SubmittedAt:      at.Add(4 * time.Minute),
	}); !errors.Is(err, ErrForbidden) {
		t.Fatalf("removed editor submit should be forbidden: %v", err)
	}

	if _, err := pool.Exec(
		context.Background(),
		`UPDATE projects SET owner_subject = 'attacker'
		 WHERE project_id = $1::uuid`,
		creation.ProjectID,
	); err == nil {
		t.Fatal("database allowed immutable project owner update")
	}
	if _, err := pool.Exec(
		context.Background(),
		`UPDATE releases SET submitted_by_subject = 'attacker'
		 WHERE release_id = $1::uuid`,
		release.ReleaseID,
	); err == nil {
		t.Fatal("database allowed immutable submitter update")
	}
	if _, err := pool.Exec(
		context.Background(),
		`UPDATE projects SET visibility = 'unlisted'
		 WHERE project_id = $1::uuid`,
		creation.ProjectID,
	); err == nil {
		t.Fatal("database allowed invalid project visibility")
	}
}

func TestPostgresMigrationBackfillsLegacyProjectOwnership(t *testing.T) {
	pool := postgresTestPool(t, false)
	applyLegacyMigration(t, pool)
	artifact, _, _ := signedArtifact(t, "f2111111-2222-3333-4444-555555555555")
	metadata, err := InspectAndVerifyEnvelope(artifact)
	if err != nil {
		t.Fatal(err)
	}
	at := time.Date(2026, 10, 15, 14, 0, 0, 0, time.UTC)
	insertLegacyRelease(t, pool, metadata, artifact, "legacy-owner", at)
	if err := migrations.Apply(context.Background(), pool); err != nil {
		t.Fatal(err)
	}
	var owner string
	var submitter string
	var visibility ProjectVisibility
	if err := pool.QueryRow(
		context.Background(),
		`SELECT projects.owner_subject, releases.submitted_by_subject,
		        projects.visibility
		 FROM projects
		 JOIN releases USING (project_id)
		 WHERE releases.release_id = $1::uuid`,
		metadata.ReleaseID,
	).Scan(&owner, &submitter, &visibility); err != nil {
		t.Fatal(err)
	}
	if owner != "legacy-owner" || submitter != "legacy-owner" ||
		visibility != ProjectVisibilityPrivate {
		t.Fatalf("legacy backfill mismatch: owner=%q submitter=%q visibility=%q", owner, submitter, visibility)
	}
}

func TestPostgresMigrationRejectsConflictingLegacyOwners(t *testing.T) {
	pool := postgresTestPool(t, false)
	applyLegacyMigration(t, pool)
	first, _, _ := signedArtifact(t, "f3111111-2222-3333-4444-555555555555")
	second, _, _ := signedArtifact(t, "f4111111-2222-3333-4444-555555555555")
	firstMetadata, _ := InspectAndVerifyEnvelope(first)
	secondMetadata, _ := InspectAndVerifyEnvelope(second)
	at := time.Date(2026, 10, 15, 15, 0, 0, 0, time.UTC)
	insertLegacyRelease(t, pool, firstMetadata, first, "legacy-owner-a", at)
	insertLegacyRelease(t, pool, secondMetadata, second, "legacy-owner-b", at.Add(time.Second))
	if err := migrations.Apply(context.Background(), pool); err == nil {
		t.Fatal("migration accepted one legacy project with multiple owners")
	}
}

func applyLegacyMigration(t *testing.T, pool *pgxpool.Pool) {
	t.Helper()
	raw, err := os.ReadFile("../../migrations/001_initial.sql")
	if err != nil {
		t.Fatal(err)
	}
	if _, err := pool.Exec(context.Background(), string(raw)); err != nil {
		t.Fatal(err)
	}
}

func insertLegacyRelease(
	t *testing.T,
	pool *pgxpool.Pool,
	metadata PackMetadata,
	artifact []byte,
	owner string,
	at time.Time,
) {
	t.Helper()
	if _, err := pool.Exec(
		context.Background(),
		`INSERT INTO releases (
			release_id, project_id, schema_version, owner_subject,
			publisher_public_key, content_hash, artifact_sha256, artifact_size,
			artifact, status, revision, submitted_at, updated_at
		) VALUES (
			$1::uuid, $2::uuid, $3, $4, $5, $6, $7, $8,
			$9, 'submitted', 1, $10, $10
		)`,
		metadata.ReleaseID,
		metadata.ProjectID,
		metadata.SchemaVersion,
		owner,
		metadata.PublisherPublicKey,
		metadata.ContentHash,
		metadata.ArtifactSHA256,
		metadata.ArtifactSize,
		artifact,
		at,
	); err != nil {
		t.Fatal(err)
	}
}

func createPostgresTestProject(
	t *testing.T,
	store *PostgresStore,
	projectID string,
	at time.Time,
) Project {
	t.Helper()
	project, created, err := store.CreateProject(context.Background(), ProjectCreation{
		ProjectID:      projectID,
		DisplayName:    "PostgreSQL test project",
		OwnerSubject:   "creator-a",
		IdempotencyKey: "postgres-create-project-v1",
		CreatedAt:      at,
	})
	if err != nil || !created {
		t.Fatalf("create PostgreSQL project failed: created=%v project=%+v err=%v", created, project, err)
	}
	return project
}

func postgresTestStore(t *testing.T) (*PostgresStore, *pgxpool.Pool) {
	t.Helper()
	pool := postgresTestPool(t, true)
	return NewPostgresStore(pool), pool
}

func postgresTestPool(t *testing.T, applyMigrations bool) *pgxpool.Pool {
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
	if applyMigrations {
		if err := migrations.Apply(ctx, pool); err != nil {
			t.Fatal(err)
		}
	}
	t.Cleanup(func() {
		pool.Close()
		cleanupContext, cleanupCancel := context.WithTimeout(context.Background(), 15*time.Second)
		defer cleanupCancel()
		_, _ = admin.Exec(cleanupContext, `DROP SCHEMA `+identifier+` CASCADE`)
		admin.Close()
	})
	return pool
}
