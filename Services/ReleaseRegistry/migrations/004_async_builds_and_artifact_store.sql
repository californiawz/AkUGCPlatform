ALTER TABLE releases
    ALTER COLUMN artifact DROP NOT NULL;

CREATE TABLE IF NOT EXISTS build_jobs (
    release_id uuid PRIMARY KEY REFERENCES releases(release_id),
    project_id uuid NOT NULL REFERENCES projects(project_id),
    status text NOT NULL CHECK (status IN ('queued', 'running', 'succeeded', 'failed')),
    attempt integer NOT NULL CHECK (attempt >= 0),
    revision bigint NOT NULL CHECK (revision >= 1),
    retryable boolean NOT NULL DEFAULT false,
    input_artifact_sha256 text NOT NULL CHECK (input_artifact_sha256 ~ '^[0-9a-f]{64}$'),
    output_artifact_sha256 text CHECK (
        output_artifact_sha256 IS NULL
        OR output_artifact_sha256 ~ '^[0-9a-f]{64}$'
    ),
    output_artifact_size bigint CHECK (
        output_artifact_size IS NULL
        OR output_artifact_size BETWEEN 1 AND 16777216
    ),
    error_code text CHECK (
        error_code IS NULL
        OR error_code IN (
            'validation_rejected',
            'validator_unavailable',
            'artifact_unavailable',
            'artifact_corrupt',
            'lease_expired'
        )
    ),
    created_at timestamptz NOT NULL,
    started_at timestamptz,
    finished_at timestamptz,
    updated_at timestamptz NOT NULL,
    lease_owner text CHECK (
        lease_owner IS NULL OR length(lease_owner) BETWEEN 1 AND 128
    ),
    lease_deadline timestamptz,
    CHECK ((status = 'running') = (lease_owner IS NOT NULL AND lease_deadline IS NOT NULL)),
    CHECK (NOT retryable OR status = 'failed'),
    CHECK (
        status <> 'succeeded'
        OR (
            output_artifact_sha256 IS NOT NULL
            AND output_artifact_size IS NOT NULL
            AND error_code IS NULL
            AND finished_at IS NOT NULL
        )
    ),
    CHECK (
        status <> 'failed'
        OR (
            output_artifact_sha256 IS NULL
            AND output_artifact_size IS NULL
            AND error_code IS NOT NULL
            AND finished_at IS NOT NULL
        )
    )
);

CREATE TABLE IF NOT EXISTS build_job_events (
    event_id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    release_id uuid NOT NULL REFERENCES build_jobs(release_id),
    actor text NOT NULL CHECK (length(actor) BETWEEN 1 AND 128),
    from_status text CHECK (from_status IN ('queued', 'running', 'succeeded', 'failed')),
    to_status text NOT NULL CHECK (to_status IN ('queued', 'running', 'succeeded', 'failed')),
    attempt integer NOT NULL CHECK (attempt >= 0),
    revision bigint NOT NULL CHECK (revision >= 1),
    error_code text CHECK (
        error_code IS NULL
        OR error_code IN (
            'validation_rejected',
            'validator_unavailable',
            'artifact_unavailable',
            'artifact_corrupt',
            'lease_expired'
        )
    ),
    occurred_at timestamptz NOT NULL
);

CREATE TABLE IF NOT EXISTS artifact_migration_events (
    release_id uuid PRIMARY KEY REFERENCES releases(release_id),
    artifact_sha256 text NOT NULL CHECK (artifact_sha256 ~ '^[0-9a-f]{64}$'),
    artifact_size bigint NOT NULL CHECK (artifact_size BETWEEN 1 AND 16777216),
    migrated_at timestamptz NOT NULL
);

INSERT INTO build_jobs (
    release_id,
    project_id,
    status,
    attempt,
    revision,
    retryable,
    input_artifact_sha256,
    output_artifact_sha256,
    output_artifact_size,
    created_at,
    started_at,
    finished_at,
    updated_at
)
SELECT
    release_id,
    project_id,
    'succeeded',
    1,
    1,
    false,
    artifact_sha256,
    artifact_sha256,
    artifact_size,
    submitted_at,
    submitted_at,
    submitted_at,
    updated_at
FROM releases
ON CONFLICT (release_id) DO NOTHING;

INSERT INTO build_job_events (
    release_id,
    actor,
    from_status,
    to_status,
    attempt,
    revision,
    occurred_at
)
SELECT
    jobs.release_id,
    'migration:p20.4',
    NULL,
    'succeeded',
    jobs.attempt,
    jobs.revision,
    jobs.updated_at
FROM build_jobs AS jobs
WHERE jobs.status = 'succeeded'
  AND NOT EXISTS (
      SELECT 1
      FROM build_job_events AS events
      WHERE events.release_id = jobs.release_id
  );

CREATE INDEX IF NOT EXISTS build_jobs_claim_idx
    ON build_jobs (created_at, release_id)
    WHERE status = 'queued';

CREATE INDEX IF NOT EXISTS build_jobs_expired_lease_idx
    ON build_jobs (lease_deadline, created_at, release_id)
    WHERE status = 'running';

CREATE INDEX IF NOT EXISTS build_job_events_release_idx
    ON build_job_events (release_id, event_id);

CREATE OR REPLACE FUNCTION reject_release_immutable_update()
RETURNS trigger
LANGUAGE plpgsql
AS $$
BEGIN
    IF NEW.release_id IS DISTINCT FROM OLD.release_id
        OR NEW.project_id IS DISTINCT FROM OLD.project_id
        OR NEW.schema_version IS DISTINCT FROM OLD.schema_version
        OR NEW.owner_subject IS DISTINCT FROM OLD.owner_subject
        OR NEW.submitted_by_subject IS DISTINCT FROM OLD.submitted_by_subject
        OR NEW.publisher_public_key IS DISTINCT FROM OLD.publisher_public_key
        OR NEW.content_hash IS DISTINCT FROM OLD.content_hash
        OR NEW.artifact_sha256 IS DISTINCT FROM OLD.artifact_sha256
        OR NEW.artifact_size IS DISTINCT FROM OLD.artifact_size
        OR NEW.submitted_at IS DISTINCT FROM OLD.submitted_at
        OR (
            NEW.artifact IS DISTINCT FROM OLD.artifact
            AND NOT (OLD.artifact IS NOT NULL AND NEW.artifact IS NULL)
        )
    THEN
        RAISE EXCEPTION 'immutable release columns cannot be changed'
            USING ERRCODE = 'integrity_constraint_violation';
    END IF;
    RETURN NEW;
END;
$$;
