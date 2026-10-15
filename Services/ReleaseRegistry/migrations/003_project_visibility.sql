ALTER TABLE projects
    ADD COLUMN IF NOT EXISTS visibility text NOT NULL DEFAULT 'private';

DO $$
BEGIN
    IF NOT EXISTS (
        SELECT 1
        FROM pg_constraint
        WHERE conname = 'projects_visibility_value'
          AND conrelid = 'projects'::regclass
    ) THEN
        ALTER TABLE projects
            ADD CONSTRAINT projects_visibility_value
            CHECK (visibility IN ('private', 'public'));
    END IF;
END;
$$;

CREATE TABLE IF NOT EXISTS project_visibility_events (
    event_id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    project_id uuid NOT NULL REFERENCES projects(project_id),
    actor_subject text NOT NULL CHECK (length(actor_subject) BETWEEN 1 AND 128),
    from_visibility text NOT NULL CHECK (from_visibility IN ('private', 'public')),
    to_visibility text NOT NULL CHECK (to_visibility IN ('private', 'public')),
    occurred_at timestamptz NOT NULL,
    CHECK (from_visibility IS DISTINCT FROM to_visibility)
);

CREATE INDEX IF NOT EXISTS project_visibility_events_project_idx
    ON project_visibility_events (project_id, event_id);

CREATE INDEX IF NOT EXISTS releases_project_published_idx
    ON releases (project_id, submitted_at DESC, release_id DESC)
    WHERE status = 'published';
