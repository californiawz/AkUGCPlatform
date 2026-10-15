CREATE TABLE IF NOT EXISTS releases (
    release_id uuid PRIMARY KEY,
    project_id uuid NOT NULL,
    schema_version integer NOT NULL CHECK (schema_version >= 0),
    owner_subject text NOT NULL CHECK (length(owner_subject) BETWEEN 1 AND 128),
    publisher_public_key text NOT NULL CHECK (publisher_public_key ~ '^[0-9a-f]{64}$'),
    content_hash text NOT NULL CHECK (content_hash ~ '^[0-9a-f]{64}$'),
    artifact_sha256 text NOT NULL CHECK (artifact_sha256 ~ '^[0-9a-f]{64}$'),
    artifact_size bigint NOT NULL CHECK (artifact_size BETWEEN 1 AND 16777216),
    artifact bytea NOT NULL CHECK (octet_length(artifact) = artifact_size),
    status text NOT NULL CHECK (status IN ('submitted', 'approved', 'rejected', 'published', 'revoked')),
    revision bigint NOT NULL CHECK (revision >= 1),
    review_reason text NOT NULL DEFAULT '' CHECK (octet_length(review_reason) <= 1024),
    submitted_at timestamptz NOT NULL,
    reviewed_at timestamptz,
    published_at timestamptz,
    revoked_at timestamptz,
    updated_at timestamptz NOT NULL
);

CREATE TABLE IF NOT EXISTS submission_idempotency (
    owner_subject text NOT NULL,
    idempotency_key text NOT NULL CHECK (length(idempotency_key) BETWEEN 16 AND 128),
    artifact_sha256 text NOT NULL CHECK (artifact_sha256 ~ '^[0-9a-f]{64}$'),
    release_id uuid NOT NULL REFERENCES releases(release_id),
    created_at timestamptz NOT NULL,
    PRIMARY KEY (owner_subject, idempotency_key)
);

CREATE TABLE IF NOT EXISTS release_transitions (
    transition_id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    release_id uuid NOT NULL REFERENCES releases(release_id),
    actor_subject text NOT NULL CHECK (length(actor_subject) BETWEEN 1 AND 128),
    from_status text NOT NULL CHECK (from_status IN ('submitted', 'approved', 'rejected', 'published', 'revoked')),
    to_status text NOT NULL CHECK (to_status IN ('submitted', 'approved', 'rejected', 'published', 'revoked')),
    reason text NOT NULL DEFAULT '' CHECK (octet_length(reason) <= 1024),
    occurred_at timestamptz NOT NULL
);

CREATE INDEX IF NOT EXISTS releases_published_updated_idx
    ON releases (updated_at DESC)
    WHERE status = 'published';

CREATE INDEX IF NOT EXISTS release_transitions_release_idx
    ON release_transitions (release_id, transition_id);

CREATE OR REPLACE FUNCTION reject_release_immutable_update()
RETURNS trigger
LANGUAGE plpgsql
AS $$
BEGIN
    IF NEW.release_id IS DISTINCT FROM OLD.release_id
        OR NEW.project_id IS DISTINCT FROM OLD.project_id
        OR NEW.schema_version IS DISTINCT FROM OLD.schema_version
        OR NEW.owner_subject IS DISTINCT FROM OLD.owner_subject
        OR NEW.publisher_public_key IS DISTINCT FROM OLD.publisher_public_key
        OR NEW.content_hash IS DISTINCT FROM OLD.content_hash
        OR NEW.artifact_sha256 IS DISTINCT FROM OLD.artifact_sha256
        OR NEW.artifact_size IS DISTINCT FROM OLD.artifact_size
        OR NEW.artifact IS DISTINCT FROM OLD.artifact
        OR NEW.submitted_at IS DISTINCT FROM OLD.submitted_at
    THEN
        RAISE EXCEPTION 'immutable release columns cannot be changed'
            USING ERRCODE = 'integrity_constraint_violation';
    END IF;
    RETURN NEW;
END;
$$;

DROP TRIGGER IF EXISTS releases_immutable_guard ON releases;
CREATE TRIGGER releases_immutable_guard
BEFORE UPDATE ON releases
FOR EACH ROW EXECUTE FUNCTION reject_release_immutable_update();
