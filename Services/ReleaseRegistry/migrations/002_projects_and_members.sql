CREATE TABLE IF NOT EXISTS projects (
    project_id uuid PRIMARY KEY,
    display_name text NOT NULL CHECK (
        octet_length(display_name) BETWEEN 1 AND 128
        AND display_name = btrim(display_name)
    ),
    owner_subject text NOT NULL CHECK (length(owner_subject) BETWEEN 1 AND 128),
    revision bigint NOT NULL CHECK (revision >= 1),
    created_at timestamptz NOT NULL,
    updated_at timestamptz NOT NULL
);

DO $$
BEGIN
    IF EXISTS (
        SELECT project_id
        FROM releases
        GROUP BY project_id
        HAVING count(DISTINCT owner_subject) > 1
    ) THEN
        RAISE EXCEPTION 'cannot migrate a project with releases owned by multiple subjects'
            USING ERRCODE = 'integrity_constraint_violation';
    END IF;
END;
$$;

INSERT INTO projects (
    project_id, display_name, owner_subject, revision, created_at, updated_at
)
SELECT
    project_id,
    'Migrated project ' || project_id::text,
    min(owner_subject),
    1,
    min(submitted_at),
    max(updated_at)
FROM releases
GROUP BY project_id
ON CONFLICT (project_id) DO NOTHING;

CREATE TABLE IF NOT EXISTS project_creation_idempotency (
    owner_subject text NOT NULL,
    idempotency_key text NOT NULL CHECK (length(idempotency_key) BETWEEN 16 AND 128),
    project_id uuid NOT NULL REFERENCES projects(project_id),
    display_name text NOT NULL CHECK (octet_length(display_name) BETWEEN 1 AND 128),
    created_at timestamptz NOT NULL,
    PRIMARY KEY (owner_subject, idempotency_key)
);

CREATE TABLE IF NOT EXISTS project_members (
    project_id uuid NOT NULL REFERENCES projects(project_id),
    subject text NOT NULL CHECK (length(subject) BETWEEN 1 AND 128),
    role text NOT NULL CHECK (role IN ('editor', 'viewer')),
    added_at timestamptz NOT NULL,
    updated_at timestamptz NOT NULL,
    PRIMARY KEY (project_id, subject)
);

CREATE TABLE IF NOT EXISTS project_member_events (
    event_id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    project_id uuid NOT NULL REFERENCES projects(project_id),
    actor_subject text NOT NULL CHECK (length(actor_subject) BETWEEN 1 AND 128),
    member_subject text NOT NULL CHECK (length(member_subject) BETWEEN 1 AND 128),
    from_role text CHECK (from_role IN ('editor', 'viewer')),
    to_role text CHECK (to_role IN ('editor', 'viewer')),
    occurred_at timestamptz NOT NULL,
    CHECK (from_role IS NOT NULL OR to_role IS NOT NULL),
    CHECK (from_role IS DISTINCT FROM to_role)
);

ALTER TABLE releases
    ADD COLUMN IF NOT EXISTS submitted_by_subject text;

UPDATE releases
SET submitted_by_subject = owner_subject
WHERE submitted_by_subject IS NULL;

ALTER TABLE releases
    ALTER COLUMN submitted_by_subject SET NOT NULL;

DO $$
BEGIN
    IF NOT EXISTS (
        SELECT 1
        FROM pg_constraint
        WHERE conname = 'releases_submitted_by_subject_length'
          AND conrelid = 'releases'::regclass
    ) THEN
        ALTER TABLE releases
            ADD CONSTRAINT releases_submitted_by_subject_length
            CHECK (length(submitted_by_subject) BETWEEN 1 AND 128);
    END IF;
    IF NOT EXISTS (
        SELECT 1
        FROM pg_constraint
        WHERE conname = 'releases_project_id_fkey'
          AND conrelid = 'releases'::regclass
    ) THEN
        ALTER TABLE releases
            ADD CONSTRAINT releases_project_id_fkey
            FOREIGN KEY (project_id) REFERENCES projects(project_id)
            NOT VALID;
    END IF;
END;
$$;

ALTER TABLE releases VALIDATE CONSTRAINT releases_project_id_fkey;

CREATE INDEX IF NOT EXISTS releases_project_versions_idx
    ON releases (project_id, submitted_at DESC, release_id DESC);

CREATE INDEX IF NOT EXISTS project_member_events_project_idx
    ON project_member_events (project_id, event_id);

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
        OR NEW.artifact IS DISTINCT FROM OLD.artifact
        OR NEW.submitted_at IS DISTINCT FROM OLD.submitted_at
    THEN
        RAISE EXCEPTION 'immutable release columns cannot be changed'
            USING ERRCODE = 'integrity_constraint_violation';
    END IF;
    RETURN NEW;
END;
$$;

CREATE OR REPLACE FUNCTION reject_project_immutable_update()
RETURNS trigger
LANGUAGE plpgsql
AS $$
BEGIN
    IF NEW.project_id IS DISTINCT FROM OLD.project_id
        OR NEW.display_name IS DISTINCT FROM OLD.display_name
        OR NEW.owner_subject IS DISTINCT FROM OLD.owner_subject
        OR NEW.created_at IS DISTINCT FROM OLD.created_at
    THEN
        RAISE EXCEPTION 'immutable project columns cannot be changed'
            USING ERRCODE = 'integrity_constraint_violation';
    END IF;
    RETURN NEW;
END;
$$;

DROP TRIGGER IF EXISTS projects_immutable_guard ON projects;
CREATE TRIGGER projects_immutable_guard
BEFORE UPDATE ON projects
FOR EACH ROW EXECUTE FUNCTION reject_project_immutable_update();

CREATE OR REPLACE FUNCTION reject_project_owner_as_member()
RETURNS trigger
LANGUAGE plpgsql
AS $$
BEGIN
    IF EXISTS (
        SELECT 1
        FROM projects
        WHERE project_id = NEW.project_id
          AND owner_subject = NEW.subject
    ) THEN
        RAISE EXCEPTION 'project owner cannot also be a mutable project member'
            USING ERRCODE = 'integrity_constraint_violation';
    END IF;
    RETURN NEW;
END;
$$;

DROP TRIGGER IF EXISTS project_members_owner_guard ON project_members;
CREATE TRIGGER project_members_owner_guard
BEFORE INSERT OR UPDATE ON project_members
FOR EACH ROW EXECUTE FUNCTION reject_project_owner_as_member();
