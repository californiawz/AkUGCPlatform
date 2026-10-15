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
	release_id::text, project_id::text, schema_version, owner_subject, submitted_by_subject,
	publisher_public_key, content_hash, artifact_sha256, artifact_size,
	status, revision, review_reason, submitted_at, reviewed_at,
	published_at, revoked_at, updated_at, artifact`

const releaseMetadataColumns = `
	release_id::text, project_id::text, schema_version, owner_subject, submitted_by_subject,
	publisher_public_key, content_hash, artifact_sha256, artifact_size,
	status, revision, review_reason, submitted_at, reviewed_at,
	published_at, revoked_at, updated_at`

type PostgresStore struct {
	pool *pgxpool.Pool
}

func NewPostgresStore(pool *pgxpool.Pool) *PostgresStore {
	return &PostgresStore{pool: pool}
}

func (s *PostgresStore) CreateProject(
	ctx context.Context,
	creation ProjectCreation,
) (Project, bool, error) {
	transaction, err := s.pool.BeginTx(ctx, pgx.TxOptions{})
	if err != nil {
		return Project{}, false, unavailable("begin project creation", err)
	}
	defer transaction.Rollback(ctx)

	lockID := creation.OwnerSubject + "\n" + creation.IdempotencyKey
	if _, err := transaction.Exec(
		ctx,
		`SELECT pg_advisory_xact_lock(hashtextextended($1, 0))`,
		lockID,
	); err != nil {
		return Project{}, false, unavailable("lock project idempotency key", err)
	}

	var previousProjectID string
	var previousDisplayName string
	err = transaction.QueryRow(
		ctx,
		`SELECT project_id::text, display_name
		 FROM project_creation_idempotency
		 WHERE owner_subject = $1 AND idempotency_key = $2`,
		creation.OwnerSubject,
		creation.IdempotencyKey,
	).Scan(&previousProjectID, &previousDisplayName)
	if err == nil {
		if previousProjectID != creation.ProjectID || previousDisplayName != creation.DisplayName {
			return Project{}, false, fmt.Errorf("%w: idempotency key was reused with different project metadata", ErrConflict)
		}
		project, err := queryProject(ctx, transaction, previousProjectID, "")
		if err != nil {
			return Project{}, false, err
		}
		if err := transaction.Commit(ctx); err != nil {
			return Project{}, false, unavailable("commit idempotent project creation", err)
		}
		return project, false, nil
	}
	if !errors.Is(err, pgx.ErrNoRows) {
		return Project{}, false, unavailable("read project idempotency key", err)
	}

	tag, err := transaction.Exec(
		ctx,
		`INSERT INTO projects (
			project_id, display_name, owner_subject, revision, created_at, updated_at
		) VALUES ($1::uuid, $2, $3, 1, $4, $4)
		ON CONFLICT (project_id) DO NOTHING`,
		creation.ProjectID,
		creation.DisplayName,
		creation.OwnerSubject,
		creation.CreatedAt,
	)
	if err != nil {
		return Project{}, false, unavailable("insert project", err)
	}
	created := tag.RowsAffected() == 1
	var project Project
	if created {
		project = Project{
			ProjectID:    creation.ProjectID,
			DisplayName:  creation.DisplayName,
			OwnerSubject: creation.OwnerSubject,
			Revision:     1,
			CreatedAt:    creation.CreatedAt,
			UpdatedAt:    creation.CreatedAt,
		}
	} else {
		project, err = queryProject(ctx, transaction, creation.ProjectID, "FOR UPDATE")
		if err != nil {
			return Project{}, false, err
		}
		if project.OwnerSubject != creation.OwnerSubject ||
			project.DisplayName != creation.DisplayName {
			return Project{}, false, fmt.Errorf("%w: project ID already identifies different immutable metadata", ErrConflict)
		}
	}

	if _, err := transaction.Exec(
		ctx,
		`INSERT INTO project_creation_idempotency (
			owner_subject, idempotency_key, project_id, display_name, created_at
		) VALUES ($1, $2, $3::uuid, $4, $5)`,
		creation.OwnerSubject,
		creation.IdempotencyKey,
		creation.ProjectID,
		creation.DisplayName,
		creation.CreatedAt,
	); err != nil {
		return Project{}, false, unavailable("insert project idempotency key", err)
	}
	if err := transaction.Commit(ctx); err != nil {
		return Project{}, false, unavailable("commit project creation", err)
	}
	return project, created, nil
}

func (s *PostgresStore) GetProject(ctx context.Context, projectID string) (Project, error) {
	return queryProject(ctx, s.pool, projectID, "")
}

func (s *PostgresStore) GetProjectRole(
	ctx context.Context,
	projectID string,
	subject string,
) (ProjectRole, bool, error) {
	project, err := queryProject(ctx, s.pool, projectID, "")
	if err != nil {
		return "", false, err
	}
	if project.OwnerSubject == subject {
		return ProjectRoleOwner, true, nil
	}
	var role ProjectRole
	err = s.pool.QueryRow(
		ctx,
		`SELECT role FROM project_members
		 WHERE project_id = $1::uuid AND subject = $2`,
		projectID,
		subject,
	).Scan(&role)
	if errors.Is(err, pgx.ErrNoRows) {
		return "", false, nil
	}
	if err != nil {
		return "", false, unavailable("query project member", err)
	}
	return role, true, nil
}

func (s *PostgresStore) ListProjectMembers(
	ctx context.Context,
	projectID string,
) ([]ProjectMember, error) {
	if _, err := queryProject(ctx, s.pool, projectID, ""); err != nil {
		return nil, err
	}
	rows, err := s.pool.Query(
		ctx,
		`SELECT project_id::text, subject, role, added_at, updated_at
		 FROM (
			SELECT project_id, owner_subject AS subject, 'owner'::text AS role,
			       created_at AS added_at, created_at AS updated_at, 0 AS sort_order
			FROM projects WHERE project_id = $1::uuid
			UNION ALL
			SELECT project_id, subject, role, added_at, updated_at, 1 AS sort_order
			FROM project_members WHERE project_id = $1::uuid
		 ) members
		 ORDER BY sort_order, subject`,
		projectID,
	)
	if err != nil {
		return nil, unavailable("list project members", err)
	}
	defer rows.Close()
	result := make([]ProjectMember, 0)
	for rows.Next() {
		var member ProjectMember
		if err := rows.Scan(
			&member.ProjectID,
			&member.Subject,
			&member.Role,
			&member.AddedAt,
			&member.UpdatedAt,
		); err != nil {
			return nil, unavailable("scan project member", err)
		}
		result = append(result, member)
	}
	if err := rows.Err(); err != nil {
		return nil, unavailable("list project members", err)
	}
	return result, nil
}

func (s *PostgresStore) SetProjectMember(
	ctx context.Context,
	change ProjectMemberChange,
) (ProjectMember, Project, bool, error) {
	transaction, err := s.pool.BeginTx(ctx, pgx.TxOptions{})
	if err != nil {
		return ProjectMember{}, Project{}, false, unavailable("begin member change", err)
	}
	defer transaction.Rollback(ctx)
	project, err := queryProject(ctx, transaction, change.ProjectID, "FOR UPDATE")
	if err != nil {
		return ProjectMember{}, Project{}, false, err
	}
	if project.OwnerSubject != change.Actor {
		return ProjectMember{}, Project{}, false, ErrNotFound
	}
	if change.Subject == project.OwnerSubject {
		return ProjectMember{}, Project{}, false, fmt.Errorf("%w: project owner membership cannot be changed", ErrConflict)
	}

	var existing ProjectMember
	err = transaction.QueryRow(
		ctx,
		`SELECT project_id::text, subject, role, added_at, updated_at
		 FROM project_members
		 WHERE project_id = $1::uuid AND subject = $2`,
		change.ProjectID,
		change.Subject,
	).Scan(
		&existing.ProjectID,
		&existing.Subject,
		&existing.Role,
		&existing.AddedAt,
		&existing.UpdatedAt,
	)
	if err == nil && existing.Role == change.Role {
		if err := transaction.Commit(ctx); err != nil {
			return ProjectMember{}, Project{}, false, unavailable("commit idempotent member change", err)
		}
		return existing, project, false, nil
	}
	if err != nil && !errors.Is(err, pgx.ErrNoRows) {
		return ProjectMember{}, Project{}, false, unavailable("read project member", err)
	}

	var member ProjectMember
	err = transaction.QueryRow(
		ctx,
		`INSERT INTO project_members (
			project_id, subject, role, added_at, updated_at
		) VALUES ($1::uuid, $2, $3, $4, $4)
		ON CONFLICT (project_id, subject) DO UPDATE SET
			role = EXCLUDED.role,
			updated_at = EXCLUDED.updated_at
		RETURNING project_id::text, subject, role, added_at, updated_at`,
		change.ProjectID,
		change.Subject,
		change.Role,
		change.ChangedAt,
	).Scan(
		&member.ProjectID,
		&member.Subject,
		&member.Role,
		&member.AddedAt,
		&member.UpdatedAt,
	)
	if err != nil {
		return ProjectMember{}, Project{}, false, unavailable("write project member", err)
	}
	project.Revision++
	project.UpdatedAt = change.ChangedAt
	if _, err := transaction.Exec(
		ctx,
		`UPDATE projects SET revision = $2, updated_at = $3
		 WHERE project_id = $1::uuid`,
		project.ProjectID,
		project.Revision,
		project.UpdatedAt,
	); err != nil {
		return ProjectMember{}, Project{}, false, unavailable("update project revision", err)
	}
	var fromRole any
	if existing.Role != "" {
		fromRole = existing.Role
	}
	if _, err := transaction.Exec(
		ctx,
		`INSERT INTO project_member_events (
			project_id, actor_subject, member_subject, from_role, to_role, occurred_at
		) VALUES ($1::uuid, $2, $3, $4, $5, $6)`,
		project.ProjectID,
		change.Actor,
		change.Subject,
		fromRole,
		change.Role,
		change.ChangedAt,
	); err != nil {
		return ProjectMember{}, Project{}, false, unavailable("insert member audit", err)
	}
	if err := transaction.Commit(ctx); err != nil {
		return ProjectMember{}, Project{}, false, unavailable("commit member change", err)
	}
	return member, project, true, nil
}

func (s *PostgresStore) RemoveProjectMember(
	ctx context.Context,
	removal ProjectMemberRemoval,
) (Project, bool, error) {
	transaction, err := s.pool.BeginTx(ctx, pgx.TxOptions{})
	if err != nil {
		return Project{}, false, unavailable("begin member removal", err)
	}
	defer transaction.Rollback(ctx)
	project, err := queryProject(ctx, transaction, removal.ProjectID, "FOR UPDATE")
	if err != nil {
		return Project{}, false, err
	}
	if project.OwnerSubject != removal.Actor {
		return Project{}, false, ErrNotFound
	}
	if removal.Subject == project.OwnerSubject {
		return Project{}, false, fmt.Errorf("%w: project owner cannot be removed", ErrConflict)
	}
	var previousRole ProjectRole
	err = transaction.QueryRow(
		ctx,
		`DELETE FROM project_members
		 WHERE project_id = $1::uuid AND subject = $2
		 RETURNING role`,
		removal.ProjectID,
		removal.Subject,
	).Scan(&previousRole)
	if errors.Is(err, pgx.ErrNoRows) {
		if err := transaction.Commit(ctx); err != nil {
			return Project{}, false, unavailable("commit idempotent member removal", err)
		}
		return project, false, nil
	}
	if err != nil {
		return Project{}, false, unavailable("remove project member", err)
	}
	project.Revision++
	project.UpdatedAt = removal.ChangedAt
	if _, err := transaction.Exec(
		ctx,
		`UPDATE projects SET revision = $2, updated_at = $3
		 WHERE project_id = $1::uuid`,
		project.ProjectID,
		project.Revision,
		project.UpdatedAt,
	); err != nil {
		return Project{}, false, unavailable("update project revision", err)
	}
	if _, err := transaction.Exec(
		ctx,
		`INSERT INTO project_member_events (
			project_id, actor_subject, member_subject, from_role, to_role, occurred_at
		) VALUES ($1::uuid, $2, $3, $4, NULL, $5)`,
		project.ProjectID,
		removal.Actor,
		removal.Subject,
		previousRole,
		removal.ChangedAt,
	); err != nil {
		return Project{}, false, unavailable("insert member removal audit", err)
	}
	if err := transaction.Commit(ctx); err != nil {
		return Project{}, false, unavailable("commit member removal", err)
	}
	return project, true, nil
}

func (s *PostgresStore) ListProjectVersions(
	ctx context.Context,
	projectID string,
	cursor string,
	limit int,
) (ProjectVersionPage, error) {
	if _, err := queryProject(ctx, s.pool, projectID, ""); err != nil {
		return ProjectVersionPage{}, err
	}
	var cursorTime time.Time
	if cursor != "" {
		err := s.pool.QueryRow(
			ctx,
			`SELECT submitted_at FROM releases
			 WHERE project_id = $1::uuid AND release_id = $2::uuid`,
			projectID,
			cursor,
		).Scan(&cursorTime)
		if errors.Is(err, pgx.ErrNoRows) {
			return ProjectVersionPage{}, fmt.Errorf("%w: version cursor is not in this project", ErrInvalidInput)
		}
		if err != nil {
			return ProjectVersionPage{}, unavailable("read version cursor", err)
		}
	}

	query := `SELECT ` + releaseMetadataColumns + `
		FROM releases WHERE project_id = $1::uuid`
	arguments := []any{projectID}
	if cursor != "" {
		query += ` AND (submitted_at, release_id) < ($2, $3::uuid)`
		arguments = append(arguments, cursorTime, cursor)
	}
	arguments = append(arguments, limit+1)
	query += fmt.Sprintf(
		` ORDER BY submitted_at DESC, release_id DESC LIMIT $%d`,
		len(arguments),
	)
	rows, err := s.pool.Query(ctx, query, arguments...)
	if err != nil {
		return ProjectVersionPage{}, unavailable("list project versions", err)
	}
	defer rows.Close()
	releases := make([]Release, 0, limit+1)
	for rows.Next() {
		release, err := scanReleaseMetadata(rows)
		if err != nil {
			return ProjectVersionPage{}, unavailable("scan project version", err)
		}
		releases = append(releases, release)
	}
	if err := rows.Err(); err != nil {
		return ProjectVersionPage{}, unavailable("list project versions", err)
	}
	page := ProjectVersionPage{Releases: releases}
	if len(page.Releases) > limit {
		page.Releases = page.Releases[:limit]
		page.NextCursor = page.Releases[len(page.Releases)-1].ReleaseID
	}
	return page, nil
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

	project, err := queryProject(
		ctx,
		transaction,
		submission.Metadata.ProjectID,
		"FOR SHARE",
	)
	if err != nil {
		return Release{}, false, err
	}
	authorized := submission.SubmitterSubject == project.OwnerSubject
	if !authorized {
		var role ProjectRole
		err := transaction.QueryRow(
			ctx,
			`SELECT role FROM project_members
			 WHERE project_id = $1::uuid AND subject = $2`,
			project.ProjectID,
			submission.SubmitterSubject,
		).Scan(&role)
		if err == nil {
			authorized = role == ProjectRoleEditor
		} else if !errors.Is(err, pgx.ErrNoRows) {
			return Release{}, false, unavailable("check project submitter", err)
		}
	}
	if !authorized {
		return Release{}, false, ErrForbidden
	}

	lockID := project.OwnerSubject + "\n" + submission.IdempotencyKey
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
		project.OwnerSubject,
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
			release_id, project_id, schema_version, owner_subject, submitted_by_subject,
			publisher_public_key, content_hash, artifact_sha256, artifact_size,
			artifact, status, revision, submitted_at, updated_at
		) VALUES (
			$1::uuid, $2::uuid, $3, $4, $5, $6, $7, $8, $9,
			$10, 'submitted', 1, $11, $11
		) ON CONFLICT (release_id) DO NOTHING`,
		metadata.ReleaseID,
		metadata.ProjectID,
		metadata.SchemaVersion,
		project.OwnerSubject,
		submission.SubmitterSubject,
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
			OwnerSubject:       project.OwnerSubject,
			SubmittedBySubject: submission.SubmitterSubject,
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
		if !sameImmutableRelease(release, submission, project.OwnerSubject) {
			return Release{}, false, fmt.Errorf("%w: release ID already identifies different immutable content", ErrConflict)
		}
	}

	if _, err := transaction.Exec(
		ctx,
		`INSERT INTO submission_idempotency (
			owner_subject, idempotency_key, artifact_sha256, release_id, created_at
		) VALUES ($1, $2, $3, $4::uuid, $5)`,
		project.OwnerSubject,
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

func queryProject(
	ctx context.Context,
	querier releaseQuerier,
	projectID string,
	lockClause string,
) (Project, error) {
	query := `SELECT project_id::text, display_name, owner_subject, revision,
	                 created_at, updated_at
	          FROM projects WHERE project_id = $1::uuid`
	if lockClause != "" {
		query += " " + lockClause
	}
	var project Project
	err := querier.QueryRow(ctx, query, projectID).Scan(
		&project.ProjectID,
		&project.DisplayName,
		&project.OwnerSubject,
		&project.Revision,
		&project.CreatedAt,
		&project.UpdatedAt,
	)
	if errors.Is(err, pgx.ErrNoRows) {
		return Project{}, ErrNotFound
	}
	if err != nil {
		return Project{}, unavailable("query project", err)
	}
	return project, nil
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
	release, err := scanRelease(querier.QueryRow(ctx, query, releaseID))
	if errors.Is(err, pgx.ErrNoRows) {
		return Release{}, ErrNotFound
	}
	if err != nil {
		return Release{}, unavailable("query release", err)
	}
	return release, nil
}

type rowScanner interface {
	Scan(destinations ...any) error
}

func scanRelease(scanner rowScanner) (Release, error) {
	var release Release
	var reviewedAt pgtype.Timestamptz
	var publishedAt pgtype.Timestamptz
	var revokedAt pgtype.Timestamptz
	err := scanner.Scan(
		&release.ReleaseID,
		&release.ProjectID,
		&release.SchemaVersion,
		&release.OwnerSubject,
		&release.SubmittedBySubject,
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
	if err != nil {
		return Release{}, err
	}
	release.ReviewedAt = pgTimePointer(reviewedAt)
	release.PublishedAt = pgTimePointer(publishedAt)
	release.RevokedAt = pgTimePointer(revokedAt)
	return release, nil
}

func scanReleaseMetadata(scanner rowScanner) (Release, error) {
	var release Release
	var reviewedAt pgtype.Timestamptz
	var publishedAt pgtype.Timestamptz
	var revokedAt pgtype.Timestamptz
	err := scanner.Scan(
		&release.ReleaseID,
		&release.ProjectID,
		&release.SchemaVersion,
		&release.OwnerSubject,
		&release.SubmittedBySubject,
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
	)
	if err != nil {
		return Release{}, err
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
