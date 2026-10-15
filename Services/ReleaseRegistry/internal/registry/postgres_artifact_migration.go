package registry

import (
	"bytes"
	"context"
	"fmt"
	"time"

	"github.com/jackc/pgx/v5"
)

// MigrateLegacyArtifacts copies legacy release bytea values into the artifact
// store, reads them back for verification, and only then clears the matching
// database value and records the migration in one transaction.
func (s *PostgresStore) MigrateLegacyArtifacts(
	ctx context.Context,
	artifacts ArtifactStore,
	now func() time.Time,
) (int, error) {
	if artifacts == nil {
		return 0, fmt.Errorf("%w: artifact store is not configured", ErrUnavailable)
	}
	if now == nil {
		now = func() time.Time { return time.Now().UTC() }
	}
	rows, err := s.pool.Query(
		ctx,
		`SELECT release_id::text
		 FROM releases
		 WHERE artifact IS NOT NULL
		 ORDER BY release_id`,
	)
	if err != nil {
		return 0, unavailable("list legacy artifacts", err)
	}
	releaseIDs := make([]string, 0)
	for rows.Next() {
		var releaseID string
		if err := rows.Scan(&releaseID); err != nil {
			rows.Close()
			return 0, unavailable("scan legacy artifact", err)
		}
		releaseIDs = append(releaseIDs, releaseID)
	}
	if err := rows.Err(); err != nil {
		rows.Close()
		return 0, unavailable("list legacy artifacts", err)
	}
	rows.Close()

	migrated := 0
	for _, releaseID := range releaseIDs {
		changed, err := s.migrateLegacyArtifact(ctx, artifacts, releaseID, now())
		if err != nil {
			return migrated, err
		}
		if changed {
			migrated++
		}
	}
	return migrated, nil
}

func (s *PostgresStore) migrateLegacyArtifact(
	ctx context.Context,
	artifacts ArtifactStore,
	releaseID string,
	at time.Time,
) (bool, error) {
	var digest string
	var size int64
	var artifact []byte
	err := s.pool.QueryRow(
		ctx,
		`SELECT artifact_sha256, artifact_size, artifact
		 FROM releases
		 WHERE release_id = $1::uuid`,
		releaseID,
	).Scan(&digest, &size, &artifact)
	if err != nil {
		return false, unavailable("read legacy artifact", err)
	}
	if artifact == nil {
		return false, nil
	}
	if int64(len(artifact)) != size {
		return false, fmt.Errorf(
			"%w: legacy artifact %s has a size mismatch",
			ErrArtifactCorrupt,
			releaseID,
		)
	}
	if err := verifyArtifact(digest, artifact); err != nil {
		return false, fmt.Errorf(
			"%w: legacy artifact %s failed digest verification: %v",
			ErrArtifactCorrupt,
			releaseID,
			err,
		)
	}
	if err := artifacts.Put(ctx, digest, artifact); err != nil {
		return false, fmt.Errorf("store legacy artifact %s: %w", releaseID, err)
	}
	verified, err := artifacts.Get(ctx, digest)
	if err != nil {
		return false, fmt.Errorf("verify stored legacy artifact %s: %w", releaseID, err)
	}
	if int64(len(verified)) != size || !bytes.Equal(verified, artifact) {
		return false, fmt.Errorf(
			"%w: stored legacy artifact %s differs from the database value",
			ErrArtifactCorrupt,
			releaseID,
		)
	}

	transaction, err := s.pool.BeginTx(ctx, pgx.TxOptions{})
	if err != nil {
		return false, unavailable("begin legacy artifact migration", err)
	}
	defer transaction.Rollback(ctx)
	var lockedDigest string
	var lockedSize int64
	var lockedArtifact []byte
	err = transaction.QueryRow(
		ctx,
		`SELECT artifact_sha256, artifact_size, artifact
		 FROM releases
		 WHERE release_id = $1::uuid
		 FOR UPDATE`,
		releaseID,
	).Scan(&lockedDigest, &lockedSize, &lockedArtifact)
	if err != nil {
		return false, unavailable("lock legacy artifact", err)
	}
	if lockedArtifact == nil {
		if err := transaction.Commit(ctx); err != nil {
			return false, unavailable("commit idempotent legacy artifact migration", err)
		}
		return false, nil
	}
	if lockedDigest != digest ||
		lockedSize != size ||
		!bytes.Equal(lockedArtifact, artifact) {
		return false, fmt.Errorf(
			"%w: legacy artifact %s changed during migration",
			ErrConflict,
			releaseID,
		)
	}
	tag, err := transaction.Exec(
		ctx,
		`UPDATE releases
		 SET artifact = NULL
		 WHERE release_id = $1::uuid
		   AND artifact_sha256 = $2
		   AND artifact_size = $3
		   AND artifact = $4`,
		releaseID,
		digest,
		size,
		artifact,
	)
	if err != nil {
		return false, unavailable("clear legacy artifact", err)
	}
	if tag.RowsAffected() != 1 {
		return false, fmt.Errorf(
			"%w: legacy artifact %s no longer matches",
			ErrConflict,
			releaseID,
		)
	}
	if _, err := transaction.Exec(
		ctx,
		`INSERT INTO artifact_migration_events (
			release_id, artifact_sha256, artifact_size, migrated_at
		) VALUES ($1::uuid, $2, $3, $4)
		ON CONFLICT (release_id) DO NOTHING`,
		releaseID,
		digest,
		size,
		at,
	); err != nil {
		return false, unavailable("audit legacy artifact migration", err)
	}
	if err := transaction.Commit(ctx); err != nil {
		return false, unavailable("commit legacy artifact migration", err)
	}
	return true, nil
}
