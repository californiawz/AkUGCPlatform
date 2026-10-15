package registry

import (
	"context"
	"errors"
	"fmt"
	"time"

	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgconn"
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

const buildColumns = `
	release_id::text, project_id::text, status, attempt, revision, retryable,
	input_artifact_sha256, output_artifact_sha256, output_artifact_size,
	error_code, created_at, started_at, finished_at, updated_at,
	lease_owner, lease_deadline`

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
			Visibility:   ProjectVisibilityPrivate,
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

func (s *PostgresStore) SetProjectVisibility(
	ctx context.Context,
	change ProjectVisibilityChange,
) (Project, bool, error) {
	transaction, err := s.pool.BeginTx(ctx, pgx.TxOptions{})
	if err != nil {
		return Project{}, false, unavailable("begin visibility change", err)
	}
	defer transaction.Rollback(ctx)
	project, err := queryProject(ctx, transaction, change.ProjectID, "FOR UPDATE")
	if err != nil {
		return Project{}, false, err
	}
	if project.OwnerSubject != change.Actor {
		return Project{}, false, ErrNotFound
	}
	if project.Visibility == change.Visibility {
		if err := transaction.Commit(ctx); err != nil {
			return Project{}, false, unavailable("commit idempotent visibility change", err)
		}
		return project, false, nil
	}
	previous := project.Visibility
	project.Visibility = change.Visibility
	project.Revision++
	project.UpdatedAt = change.ChangedAt
	if _, err := transaction.Exec(
		ctx,
		`UPDATE projects SET visibility = $2, revision = $3, updated_at = $4
		 WHERE project_id = $1::uuid`,
		project.ProjectID,
		project.Visibility,
		project.Revision,
		project.UpdatedAt,
	); err != nil {
		return Project{}, false, unavailable("update project visibility", err)
	}
	if _, err := transaction.Exec(
		ctx,
		`INSERT INTO project_visibility_events (
			project_id, actor_subject, from_visibility, to_visibility, occurred_at
		) VALUES ($1::uuid, $2, $3, $4, $5)`,
		project.ProjectID,
		change.Actor,
		previous,
		project.Visibility,
		change.ChangedAt,
	); err != nil {
		return Project{}, false, unavailable("insert visibility audit", err)
	}
	if err := transaction.Commit(ctx); err != nil {
		return Project{}, false, unavailable("commit visibility change", err)
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

func (s *PostgresStore) GetLatestPublishedRelease(
	ctx context.Context,
	projectID string,
) (Release, error) {
	query := `SELECT ` + prefixedReleaseColumns("releases") + `
		FROM releases
		JOIN build_jobs ON build_jobs.release_id = releases.release_id
		WHERE releases.project_id = $1::uuid
		  AND releases.status = 'published'
		  AND build_jobs.status = 'succeeded'
		ORDER BY releases.submitted_at DESC, releases.release_id DESC
		LIMIT 1`
	release, err := scanRelease(s.pool.QueryRow(ctx, query, projectID))
	if errors.Is(err, pgx.ErrNoRows) {
		return Release{}, ErrNotFound
	}
	if err != nil {
		return Release{}, unavailable("query latest published release", err)
	}
	return release, nil
}

func (s *PostgresStore) Submit(
	ctx context.Context,
	submission Submission,
) (Release, BuildJob, bool, error) {
	transaction, err := s.pool.BeginTx(ctx, pgx.TxOptions{})
	if err != nil {
		return Release{}, BuildJob{}, false, unavailable("begin submission", err)
	}
	defer transaction.Rollback(ctx)

	project, err := queryProject(
		ctx,
		transaction,
		submission.Metadata.ProjectID,
		"FOR SHARE",
	)
	if err != nil {
		return Release{}, BuildJob{}, false, err
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
			return Release{}, BuildJob{}, false, unavailable("check project submitter", err)
		}
	}
	if !authorized {
		return Release{}, BuildJob{}, false, ErrForbidden
	}

	lockID := project.OwnerSubject + "\n" + submission.IdempotencyKey
	if _, err := transaction.Exec(
		ctx,
		`SELECT pg_advisory_xact_lock(hashtextextended($1, 0))`,
		lockID,
	); err != nil {
		return Release{}, BuildJob{}, false, unavailable("lock idempotency key", err)
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
			return Release{}, BuildJob{}, false, fmt.Errorf("%w: idempotency key was reused with different content", ErrConflict)
		}
		release, err := queryRelease(ctx, transaction, previousReleaseID, false)
		if err != nil {
			return Release{}, BuildJob{}, false, err
		}
		build, err := queryBuild(ctx, transaction, previousReleaseID, false)
		if err != nil {
			return Release{}, BuildJob{}, false, err
		}
		if err := transaction.Commit(ctx); err != nil {
			return Release{}, BuildJob{}, false, unavailable("commit idempotent submission", err)
		}
		return release, build, false, nil
	}
	if !errors.Is(err, pgx.ErrNoRows) {
		return Release{}, BuildJob{}, false, unavailable("read idempotency key", err)
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
			NULL, 'submitted', 1, $10, $10
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
		submission.SubmittedAt,
	)
	if err != nil {
		return Release{}, BuildJob{}, false, unavailable("insert release", err)
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
		}
	} else {
		release, err = queryRelease(ctx, transaction, metadata.ReleaseID, true)
		if err != nil {
			return Release{}, BuildJob{}, false, err
		}
		if !sameImmutableRelease(release, submission, project.OwnerSubject) {
			return Release{}, BuildJob{}, false, fmt.Errorf("%w: release ID already identifies different immutable content", ErrConflict)
		}
	}

	var build BuildJob
	if created {
		build = BuildJob{
			ReleaseID:           release.ReleaseID,
			ProjectID:           release.ProjectID,
			Status:              BuildStatusQueued,
			Attempt:             0,
			Revision:            1,
			InputArtifactSHA256: release.ArtifactSHA256,
			CreatedAt:           submission.SubmittedAt,
			UpdatedAt:           submission.SubmittedAt,
		}
		if _, err := transaction.Exec(
			ctx,
			`INSERT INTO build_jobs (
				release_id, project_id, status, attempt, revision, retryable,
				input_artifact_sha256, created_at, updated_at
			) VALUES (
				$1::uuid, $2::uuid, 'queued', 0, 1, false, $3, $4, $4
			)`,
			build.ReleaseID,
			build.ProjectID,
			build.InputArtifactSHA256,
			build.CreatedAt,
		); err != nil {
			return Release{}, BuildJob{}, false, unavailable("insert build job", err)
		}
		if err := insertBuildEvent(ctx, transaction, BuildJobEvent{
			ReleaseID: build.ReleaseID,
			Actor:     submission.SubmitterSubject,
			To:        build.Status,
			Attempt:   build.Attempt,
			Revision:  build.Revision,
			At:        submission.SubmittedAt,
		}); err != nil {
			return Release{}, BuildJob{}, false, err
		}
	} else {
		build, err = queryBuild(ctx, transaction, release.ReleaseID, false)
		if err != nil {
			return Release{}, BuildJob{}, false, err
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
		return Release{}, BuildJob{}, false, unavailable("insert idempotency key", err)
	}
	if err := transaction.Commit(ctx); err != nil {
		return Release{}, BuildJob{}, false, unavailable("commit submission", err)
	}
	return release, build, created, nil
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
	if transitionRequiresSucceededBuild(request.Target) {
		build, buildErr := queryBuild(ctx, transaction, request.ReleaseID, false)
		if buildErr != nil {
			return Release{}, false, buildErr
		}
		if build.Status != BuildStatusSucceeded {
			return Release{}, false, fmt.Errorf("%w: release build has not succeeded", ErrConflict)
		}
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

func (s *PostgresStore) GetBuild(
	ctx context.Context,
	releaseID string,
) (BuildJob, error) {
	return queryBuild(ctx, s.pool, releaseID, false)
}

func (s *PostgresStore) ClaimBuild(
	ctx context.Context,
	request ClaimBuildRequest,
) (BuildJob, error) {
	if request.Worker == "" || len(request.Worker) > 128 || request.LeaseDuration <= 0 {
		return BuildJob{}, fmt.Errorf("%w: invalid build lease", ErrInvalidInput)
	}
	transaction, err := s.pool.BeginTx(ctx, pgx.TxOptions{})
	if err != nil {
		return BuildJob{}, unavailable("begin build claim", err)
	}
	defer transaction.Rollback(ctx)

	for {
		query := `SELECT ` + buildColumns + `
			FROM build_jobs
			WHERE status = 'queued'
			   OR (status = 'running' AND lease_deadline <= $1)
			ORDER BY created_at, release_id
			LIMIT 1
			FOR UPDATE SKIP LOCKED`
		build, err := scanBuild(transaction.QueryRow(ctx, query, request.At))
		if errors.Is(err, pgx.ErrNoRows) {
			if err := transaction.Commit(ctx); err != nil {
				return BuildJob{}, unavailable("commit empty build claim", err)
			}
			return BuildJob{}, ErrNoBuildAvailable
		}
		if err != nil {
			return BuildJob{}, unavailable("select build claim", err)
		}
		expired := build.Status == BuildStatusRunning
		if expired && build.Attempt >= MaxAutomaticBuildAttempts {
			previous := build.Status
			build.Status = BuildStatusFailed
			build.Retryable = true
			build.ErrorCode = BuildErrorLeaseExpired
			build.Revision++
			build.FinishedAt = timePointer(request.At)
			build.UpdatedAt = request.At
			build.leaseOwner = ""
			build.leaseDeadline = nil
			if _, err := transaction.Exec(
				ctx,
				`UPDATE build_jobs SET
					status = 'failed',
					revision = $2,
					retryable = true,
					output_artifact_sha256 = NULL,
					output_artifact_size = NULL,
					error_code = 'lease_expired',
					finished_at = $3,
					updated_at = $3,
					lease_owner = NULL,
					lease_deadline = NULL
				 WHERE release_id = $1::uuid`,
				build.ReleaseID,
				build.Revision,
				request.At,
			); err != nil {
				return BuildJob{}, unavailable("expire build lease", err)
			}
			if err := insertBuildEvent(ctx, transaction, BuildJobEvent{
				ReleaseID: build.ReleaseID,
				Actor:     request.Worker,
				From:      previous,
				To:        build.Status,
				Attempt:   build.Attempt,
				Revision:  build.Revision,
				ErrorCode: build.ErrorCode,
				At:        request.At,
			}); err != nil {
				return BuildJob{}, err
			}
			continue
		}

		previous := build.Status
		build.Status = BuildStatusRunning
		build.Attempt++
		build.Revision++
		build.Retryable = false
		build.OutputArtifactSHA256 = ""
		build.OutputArtifactSize = 0
		build.ErrorCode = ""
		build.FinishedAt = nil
		if build.StartedAt == nil {
			build.StartedAt = timePointer(request.At)
		}
		build.UpdatedAt = request.At
		build.leaseOwner = request.Worker
		deadline := request.At.Add(request.LeaseDuration)
		build.leaseDeadline = &deadline
		if _, err := transaction.Exec(
			ctx,
			`UPDATE build_jobs SET
				status = 'running',
				attempt = $2,
				revision = $3,
				retryable = false,
				output_artifact_sha256 = NULL,
				output_artifact_size = NULL,
				error_code = NULL,
				started_at = COALESCE(started_at, $4),
				finished_at = NULL,
				updated_at = $4,
				lease_owner = $5,
				lease_deadline = $6
			 WHERE release_id = $1::uuid`,
			build.ReleaseID,
			build.Attempt,
			build.Revision,
			request.At,
			request.Worker,
			deadline,
		); err != nil {
			return BuildJob{}, unavailable("claim build", err)
		}
		event := BuildJobEvent{
			ReleaseID: build.ReleaseID,
			Actor:     request.Worker,
			From:      previous,
			To:        build.Status,
			Attempt:   build.Attempt,
			Revision:  build.Revision,
			At:        request.At,
		}
		if expired {
			event.ErrorCode = BuildErrorLeaseExpired
		}
		if err := insertBuildEvent(ctx, transaction, event); err != nil {
			return BuildJob{}, err
		}
		if err := transaction.Commit(ctx); err != nil {
			return BuildJob{}, unavailable("commit build claim", err)
		}
		return build, nil
	}
}

func (s *PostgresStore) CompleteBuild(
	ctx context.Context,
	request CompleteBuildRequest,
) (BuildJob, bool, error) {
	if request.Worker == "" {
		return BuildJob{}, false, fmt.Errorf("%w: build worker is required", ErrInvalidInput)
	}
	transaction, err := s.pool.BeginTx(ctx, pgx.TxOptions{})
	if err != nil {
		return BuildJob{}, false, unavailable("begin build completion", err)
	}
	defer transaction.Rollback(ctx)
	release, err := queryRelease(ctx, transaction, request.ReleaseID, false)
	if err != nil {
		return BuildJob{}, false, err
	}
	build, err := queryBuild(ctx, transaction, request.ReleaseID, true)
	if err != nil {
		return BuildJob{}, false, err
	}
	if sameBuildCompletion(build, request) {
		if err := transaction.Commit(ctx); err != nil {
			return BuildJob{}, false, unavailable("commit idempotent build completion", err)
		}
		return build, false, nil
	}
	if build.Status != BuildStatusRunning ||
		build.leaseOwner != request.Worker ||
		build.leaseDeadline == nil ||
		!build.leaseDeadline.After(request.At) {
		return BuildJob{}, false, fmt.Errorf("%w: build lease is not current", ErrConflict)
	}
	previous := build.Status
	if request.Succeeded {
		if request.ErrorCode != "" ||
			request.OutputArtifactSHA256 != release.ArtifactSHA256 ||
			request.OutputArtifactSize != release.ArtifactSize {
			return BuildJob{}, false, fmt.Errorf("%w: build output does not match immutable release metadata", ErrConflict)
		}
		build.Status = BuildStatusSucceeded
		build.Retryable = false
		build.OutputArtifactSHA256 = request.OutputArtifactSHA256
		build.OutputArtifactSize = request.OutputArtifactSize
		build.ErrorCode = ""
	} else {
		if !request.ErrorCode.Valid() ||
			request.OutputArtifactSHA256 != "" ||
			request.OutputArtifactSize != 0 {
			return BuildJob{}, false, fmt.Errorf("%w: invalid failed build completion", ErrInvalidInput)
		}
		build.Status = BuildStatusFailed
		build.Retryable = request.ErrorCode.IsRetryable()
		build.OutputArtifactSHA256 = ""
		build.OutputArtifactSize = 0
		build.ErrorCode = request.ErrorCode
	}
	build.Revision++
	build.FinishedAt = timePointer(request.At)
	build.UpdatedAt = request.At
	build.leaseDeadline = nil
	var outputDigest any
	var outputSize any
	var errorCode any
	if build.OutputArtifactSHA256 != "" {
		outputDigest = build.OutputArtifactSHA256
		outputSize = build.OutputArtifactSize
	}
	if build.ErrorCode != "" {
		errorCode = build.ErrorCode
	}
	if _, err := transaction.Exec(
		ctx,
		`UPDATE build_jobs SET
			status = $2,
			revision = $3,
			retryable = $4,
			output_artifact_sha256 = $5,
			output_artifact_size = $6,
			error_code = $7,
			finished_at = $8,
			updated_at = $8,
			lease_deadline = NULL
		 WHERE release_id = $1::uuid`,
		build.ReleaseID,
		build.Status,
		build.Revision,
		build.Retryable,
		outputDigest,
		outputSize,
		errorCode,
		request.At,
	); err != nil {
		return BuildJob{}, false, unavailable("complete build", err)
	}
	if err := insertBuildEvent(ctx, transaction, BuildJobEvent{
		ReleaseID: build.ReleaseID,
		Actor:     request.Worker,
		From:      previous,
		To:        build.Status,
		Attempt:   build.Attempt,
		Revision:  build.Revision,
		ErrorCode: build.ErrorCode,
		At:        request.At,
	}); err != nil {
		return BuildJob{}, false, err
	}
	if err := transaction.Commit(ctx); err != nil {
		return BuildJob{}, false, unavailable("commit build completion", err)
	}
	return build, true, nil
}

func (s *PostgresStore) RetryBuild(
	ctx context.Context,
	request RetryBuildRequest,
) (BuildJob, bool, error) {
	if request.Actor == "" {
		return BuildJob{}, false, fmt.Errorf("%w: retry actor is required", ErrInvalidInput)
	}
	transaction, err := s.pool.BeginTx(ctx, pgx.TxOptions{})
	if err != nil {
		return BuildJob{}, false, unavailable("begin build retry", err)
	}
	defer transaction.Rollback(ctx)
	build, err := queryBuild(ctx, transaction, request.ReleaseID, true)
	if err != nil {
		return BuildJob{}, false, err
	}
	if build.Status == BuildStatusQueued && build.Attempt == 0 {
		if err := transaction.Commit(ctx); err != nil {
			return BuildJob{}, false, unavailable("commit idempotent build retry", err)
		}
		return build, false, nil
	}
	if build.Status != BuildStatusFailed || !build.Retryable {
		return BuildJob{}, false, fmt.Errorf("%w: build is not retryable", ErrConflict)
	}
	previous := build.Status
	build.Status = BuildStatusQueued
	build.Attempt = 0
	build.Revision++
	build.Retryable = false
	build.OutputArtifactSHA256 = ""
	build.OutputArtifactSize = 0
	build.ErrorCode = ""
	build.StartedAt = nil
	build.FinishedAt = nil
	build.UpdatedAt = request.At
	build.leaseOwner = ""
	build.leaseDeadline = nil
	if _, err := transaction.Exec(
		ctx,
		`UPDATE build_jobs SET
			status = 'queued',
			attempt = 0,
			revision = $2,
			retryable = false,
			output_artifact_sha256 = NULL,
			output_artifact_size = NULL,
			error_code = NULL,
			started_at = NULL,
			finished_at = NULL,
			updated_at = $3,
			lease_owner = NULL,
			lease_deadline = NULL
		 WHERE release_id = $1::uuid`,
		build.ReleaseID,
		build.Revision,
		request.At,
	); err != nil {
		return BuildJob{}, false, unavailable("retry build", err)
	}
	if err := insertBuildEvent(ctx, transaction, BuildJobEvent{
		ReleaseID: build.ReleaseID,
		Actor:     request.Actor,
		From:      previous,
		To:        build.Status,
		Attempt:   build.Attempt,
		Revision:  build.Revision,
		At:        request.At,
	}); err != nil {
		return BuildJob{}, false, err
	}
	if err := transaction.Commit(ctx); err != nil {
		return BuildJob{}, false, unavailable("commit build retry", err)
	}
	return build, true, nil
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
	query := `SELECT project_id::text, display_name, owner_subject, visibility, revision,
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
		&project.Visibility,
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

func queryBuild(
	ctx context.Context,
	querier releaseQuerier,
	releaseID string,
	forUpdate bool,
) (BuildJob, error) {
	query := `SELECT ` + buildColumns + `
		FROM build_jobs WHERE release_id = $1::uuid`
	if forUpdate {
		query += ` FOR UPDATE`
	}
	build, err := scanBuild(querier.QueryRow(ctx, query, releaseID))
	if errors.Is(err, pgx.ErrNoRows) {
		return BuildJob{}, ErrNotFound
	}
	if err != nil {
		return BuildJob{}, unavailable("query build", err)
	}
	return build, nil
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

func scanBuild(scanner rowScanner) (BuildJob, error) {
	var build BuildJob
	var outputDigest pgtype.Text
	var outputSize pgtype.Int8
	var errorCode pgtype.Text
	var startedAt pgtype.Timestamptz
	var finishedAt pgtype.Timestamptz
	var leaseOwner pgtype.Text
	var leaseDeadline pgtype.Timestamptz
	err := scanner.Scan(
		&build.ReleaseID,
		&build.ProjectID,
		&build.Status,
		&build.Attempt,
		&build.Revision,
		&build.Retryable,
		&build.InputArtifactSHA256,
		&outputDigest,
		&outputSize,
		&errorCode,
		&build.CreatedAt,
		&startedAt,
		&finishedAt,
		&build.UpdatedAt,
		&leaseOwner,
		&leaseDeadline,
	)
	if err != nil {
		return BuildJob{}, err
	}
	if outputDigest.Valid {
		build.OutputArtifactSHA256 = outputDigest.String
	}
	if outputSize.Valid {
		build.OutputArtifactSize = outputSize.Int64
	}
	if errorCode.Valid {
		build.ErrorCode = BuildErrorCode(errorCode.String)
	}
	build.StartedAt = pgTimePointer(startedAt)
	build.FinishedAt = pgTimePointer(finishedAt)
	if leaseOwner.Valid {
		build.leaseOwner = leaseOwner.String
	}
	build.leaseDeadline = pgTimePointer(leaseDeadline)
	return build, nil
}

type commandExecutor interface {
	Exec(ctx context.Context, sql string, arguments ...any) (pgconn.CommandTag, error)
}

func insertBuildEvent(
	ctx context.Context,
	executor commandExecutor,
	event BuildJobEvent,
) error {
	var from any
	var errorCode any
	if event.From != "" {
		from = event.From
	}
	if event.ErrorCode != "" {
		errorCode = event.ErrorCode
	}
	if _, err := executor.Exec(
		ctx,
		`INSERT INTO build_job_events (
			release_id, actor, from_status, to_status,
			attempt, revision, error_code, occurred_at
		) VALUES ($1::uuid, $2, $3, $4, $5, $6, $7, $8)`,
		event.ReleaseID,
		event.Actor,
		from,
		event.To,
		event.Attempt,
		event.Revision,
		errorCode,
		event.At,
	); err != nil {
		return unavailable("insert build audit", err)
	}
	return nil
}

func prefixedReleaseColumns(alias string) string {
	return alias + `.release_id::text, ` +
		alias + `.project_id::text, ` +
		alias + `.schema_version, ` +
		alias + `.owner_subject, ` +
		alias + `.submitted_by_subject, ` +
		alias + `.publisher_public_key, ` +
		alias + `.content_hash, ` +
		alias + `.artifact_sha256, ` +
		alias + `.artifact_size, ` +
		alias + `.status, ` +
		alias + `.revision, ` +
		alias + `.review_reason, ` +
		alias + `.submitted_at, ` +
		alias + `.reviewed_at, ` +
		alias + `.published_at, ` +
		alias + `.revoked_at, ` +
		alias + `.updated_at, ` +
		alias + `.artifact`
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
