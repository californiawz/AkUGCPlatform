package registry

import (
	"context"
	"errors"
	"fmt"
	"time"

	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgtype"
	"github.com/jackc/pgx/v5/pgxpool"
)

const releaseColumns = `
	release_id::text, project_id::text, schema_version, owner_subject,
	publisher_public_key, content_hash, artifact_sha256, artifact_size,
	status, revision, review_reason, submitted_at, reviewed_at,
	published_at, revoked_at, updated_at, artifact`

type PostgresStore struct {
	pool *pgxpool.Pool
}

func NewPostgresStore(pool *pgxpool.Pool) *PostgresStore {
	return &PostgresStore{pool: pool}
}

func (s *PostgresStore) Submit(
	ctx context.Context,
	submission Submission,
) (Release, bool, error) {
	transaction, err := s.pool.BeginTx(ctx, pgx.TxOptions{})
	if err != nil {
		return Release{}, false, unavailable("begin submission", err)
	}
	defer transaction.Rollback(ctx)

	lockID := submission.OwnerSubject + "\n" + submission.IdempotencyKey
	if _, err := transaction.Exec(
		ctx,
		`SELECT pg_advisory_xact_lock(hashtextextended($1, 0))`,
		lockID,
	); err != nil {
		return Release{}, false, unavailable("lock idempotency key", err)
	}

	var previousDigest string
	var previousReleaseID string
	err = transaction.QueryRow(
		ctx,
		`SELECT artifact_sha256, release_id::text
		 FROM submission_idempotency
		 WHERE owner_subject = $1 AND idempotency_key = $2`,
		submission.OwnerSubject,
		submission.IdempotencyKey,
	).Scan(&previousDigest, &previousReleaseID)
	if err == nil {
		if previousDigest != submission.Metadata.ArtifactSHA256 {
			return Release{}, false, fmt.Errorf("%w: idempotency key was reused with different content", ErrConflict)
		}
		release, err := queryRelease(ctx, transaction, previousReleaseID, false)
		if err != nil {
			return Release{}, false, err
		}
		if err := transaction.Commit(ctx); err != nil {
			return Release{}, false, unavailable("commit idempotent submission", err)
		}
		return release, false, nil
	}
	if !errors.Is(err, pgx.ErrNoRows) {
		return Release{}, false, unavailable("read idempotency key", err)
	}

	metadata := submission.Metadata
	tag, err := transaction.Exec(
		ctx,
		`INSERT INTO releases (
			release_id, project_id, schema_version, owner_subject,
			publisher_public_key, content_hash, artifact_sha256, artifact_size,
			artifact, status, revision, submitted_at, updated_at
		) VALUES (
			$1::uuid, $2::uuid, $3, $4, $5, $6, $7, $8,
			$9, 'submitted', 1, $10, $10
		) ON CONFLICT (release_id) DO NOTHING`,
		metadata.ReleaseID,
		metadata.ProjectID,
		metadata.SchemaVersion,
		submission.OwnerSubject,
		metadata.PublisherPublicKey,
		metadata.ContentHash,
		metadata.ArtifactSHA256,
		metadata.ArtifactSize,
		submission.Artifact,
		submission.SubmittedAt,
	)
	if err != nil {
		return Release{}, false, unavailable("insert release", err)
	}
	created := tag.RowsAffected() == 1

	var release Release
	if created {
		release = Release{
			ReleaseID:          metadata.ReleaseID,
			ProjectID:          metadata.ProjectID,
			SchemaVersion:      metadata.SchemaVersion,
			OwnerSubject:       submission.OwnerSubject,
			PublisherPublicKey: metadata.PublisherPublicKey,
			ContentHash:        metadata.ContentHash,
			ArtifactSHA256:     metadata.ArtifactSHA256,
			ArtifactSize:       metadata.ArtifactSize,
			Status:             StatusSubmitted,
			Revision:           1,
			SubmittedAt:        submission.SubmittedAt,
			UpdatedAt:          submission.SubmittedAt,
			Artifact:           append([]byte(nil), submission.Artifact...),
		}
	} else {
		release, err = queryRelease(ctx, transaction, metadata.ReleaseID, true)
		if err != nil {
			return Release{}, false, err
		}
		if !sameImmutableRelease(release, submission) {
			return Release{}, false, fmt.Errorf("%w: release ID already identifies different immutable content", ErrConflict)
		}
	}

	if _, err := transaction.Exec(
		ctx,
		`INSERT INTO submission_idempotency (
			owner_subject, idempotency_key, artifact_sha256, release_id, created_at
		) VALUES ($1, $2, $3, $4::uuid, $5)`,
		submission.OwnerSubject,
		submission.IdempotencyKey,
		metadata.ArtifactSHA256,
		metadata.ReleaseID,
		submission.SubmittedAt,
	); err != nil {
		return Release{}, false, unavailable("insert idempotency key", err)
	}
	if err := transaction.Commit(ctx); err != nil {
		return Release{}, false, unavailable("commit submission", err)
	}
	return release, created, nil
}

func (s *PostgresStore) Get(ctx context.Context, releaseID string) (Release, error) {
	return queryRelease(ctx, s.pool, releaseID, false)
}

func (s *PostgresStore) Transition(
	ctx context.Context,
	request TransitionRequest,
) (Release, bool, error) {
	if !request.Target.Valid() {
		return Release{}, false, fmt.Errorf("%w: invalid target status", ErrInvalidInput)
	}
	transaction, err := s.pool.BeginTx(ctx, pgx.TxOptions{})
	if err != nil {
		return Release{}, false, unavailable("begin transition", err)
	}
	defer transaction.Rollback(ctx)

	release, err := queryRelease(ctx, transaction, request.ReleaseID, true)
	if err != nil {
		return Release{}, false, err
	}
	if release.Status == request.Target {
		if err := transaction.Commit(ctx); err != nil {
			return Release{}, false, unavailable("commit idempotent transition", err)
		}
		return release, false, nil
	}
	if !canTransition(release.Status, request.Target) {
		return Release{}, false, fmt.Errorf(
			"%w: cannot transition release from %s to %s",
			ErrConflict,
			release.Status,
			request.Target,
		)
	}

	previous := release.Status
	release.Status = request.Target
	release.Revision++
	release.UpdatedAt = request.At
	switch request.Target {
	case StatusApproved, StatusRejected:
		release.ReviewedAt = timePointer(request.At)
		release.ReviewReason = request.Reason
	case StatusPublished:
		release.PublishedAt = timePointer(request.At)
	case StatusRevoked:
		release.RevokedAt = timePointer(request.At)
	}
	if _, err := transaction.Exec(
		ctx,
		`UPDATE releases SET
			status = $2,
			revision = $3,
			review_reason = $4,
			reviewed_at = $5,
			published_at = $6,
			revoked_at = $7,
			updated_at = $8
		 WHERE release_id = $1::uuid`,
		release.ReleaseID,
		release.Status,
		release.Revision,
		release.ReviewReason,
		release.ReviewedAt,
		release.PublishedAt,
		release.RevokedAt,
		release.UpdatedAt,
	); err != nil {
		return Release{}, false, unavailable("update release state", err)
	}
	if _, err := transaction.Exec(
		ctx,
		`INSERT INTO release_transitions (
			release_id, actor_subject, from_status, to_status, reason, occurred_at
		) VALUES ($1::uuid, $2, $3, $4, $5, $6)`,
		release.ReleaseID,
		request.Actor,
		previous,
		release.Status,
		request.Reason,
		request.At,
	); err != nil {
		return Release{}, false, unavailable("insert transition audit", err)
	}
	if err := transaction.Commit(ctx); err != nil {
		return Release{}, false, unavailable("commit transition", err)
	}
	return release, true, nil
}

type releaseQuerier interface {
	QueryRow(ctx context.Context, sql string, args ...any) pgx.Row
}

func queryRelease(
	ctx context.Context,
	querier releaseQuerier,
	releaseID string,
	forUpdate bool,
) (Release, error) {
	query := `SELECT ` + releaseColumns + ` FROM releases WHERE release_id = $1::uuid`
	if forUpdate {
		query += ` FOR UPDATE`
	}
	var release Release
	var reviewedAt pgtype.Timestamptz
	var publishedAt pgtype.Timestamptz
	var revokedAt pgtype.Timestamptz
	err := querier.QueryRow(ctx, query, releaseID).Scan(
		&release.ReleaseID,
		&release.ProjectID,
		&release.SchemaVersion,
		&release.OwnerSubject,
		&release.PublisherPublicKey,
		&release.ContentHash,
		&release.ArtifactSHA256,
		&release.ArtifactSize,
		&release.Status,
		&release.Revision,
		&release.ReviewReason,
		&release.SubmittedAt,
		&reviewedAt,
		&publishedAt,
		&revokedAt,
		&release.UpdatedAt,
		&release.Artifact,
	)
	if errors.Is(err, pgx.ErrNoRows) {
		return Release{}, ErrNotFound
	}
	if err != nil {
		return Release{}, unavailable("query release", err)
	}
	release.ReviewedAt = pgTimePointer(reviewedAt)
	release.PublishedAt = pgTimePointer(publishedAt)
	release.RevokedAt = pgTimePointer(revokedAt)
	return release, nil
}

func pgTimePointer(value pgtype.Timestamptz) *time.Time {
	if !value.Valid {
		return nil
	}
	result := value.Time
	return &result
}

func unavailable(operation string, err error) error {
	return fmt.Errorf("%w: %s: %v", ErrUnavailable, operation, err)
}
