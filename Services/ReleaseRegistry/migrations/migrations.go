package migrations

import (
	"context"
	"embed"
	"fmt"
	"io/fs"
	"sort"

	"github.com/jackc/pgx/v5/pgxpool"
)

//go:embed *.sql
var files embed.FS

func Apply(ctx context.Context, pool *pgxpool.Pool) error {
	entries, err := fs.Glob(files, "*.sql")
	if err != nil {
		return fmt.Errorf("list migrations: %w", err)
	}
	sort.Strings(entries)
	for _, name := range entries {
		sql, err := files.ReadFile(name)
		if err != nil {
			return fmt.Errorf("read migration %s: %w", name, err)
		}
		if _, err := pool.Exec(ctx, string(sql)); err != nil {
			return fmt.Errorf("apply migration %s: %w", name, err)
		}
	}
	return nil
}
