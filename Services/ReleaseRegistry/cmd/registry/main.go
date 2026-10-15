package main

import (
	"context"
	"errors"
	"log"
	"net"
	"net/http"
	"os"
	"os/signal"
	"path/filepath"
	"strings"
	"syscall"
	"time"

	"akugcplatform/services/releaseregistry/internal/registry"
	"akugcplatform/services/releaseregistry/migrations"
	"github.com/jackc/pgx/v5/pgxpool"
)

func main() {
	if err := run(); err != nil {
		log.Fatal(err)
	}
}

func run() error {
	authPath := os.Getenv("AKUGC_REGISTRY_AUTH_CONFIG")
	if authPath == "" {
		return errors.New("AKUGC_REGISTRY_AUTH_CONFIG is required")
	}
	authenticator, err := registry.LoadStaticAuthenticator(authPath)
	if err != nil {
		return err
	}

	validatorExecutable, err := requiredFile("AKUGC_VALIDATOR_EXE")
	if err != nil {
		return err
	}
	project, err := requiredFile("AKUGC_VALIDATOR_PROJECT")
	if err != nil {
		return err
	}
	verifier := registry.VerifiedPackVerifier{
		Deep: registry.NewCommandValidator(
			validatorExecutable,
			project,
			os.Getenv("AKUGC_VALIDATOR_TEMP"),
			2,
			90*time.Second,
		),
	}

	var store registry.Store
	var pool *pgxpool.Pool
	if os.Getenv("AKUGC_REGISTRY_MEMORY") == "1" {
		log.Print("Release Registry is using volatile memory storage for local simulation")
		store = registry.NewMemoryStore()
	} else {
		databaseURL := os.Getenv("DATABASE_URL")
		if databaseURL == "" {
			return errors.New("DATABASE_URL is required unless AKUGC_REGISTRY_MEMORY=1")
		}
		poolConfig, err := pgxpool.ParseConfig(databaseURL)
		if err != nil {
			return errors.New("DATABASE_URL is invalid")
		}
		poolConfig.MaxConns = 16
		poolConfig.MinConns = 1
		startupContext, cancel := context.WithTimeout(context.Background(), 30*time.Second)
		defer cancel()
		pool, err = pgxpool.NewWithConfig(startupContext, poolConfig)
		if err != nil {
			return errors.New("cannot create PostgreSQL pool")
		}
		defer pool.Close()
		if err := pool.Ping(startupContext); err != nil {
			return errors.New("PostgreSQL is unavailable")
		}
		if err := migrations.Apply(startupContext, pool); err != nil {
			return err
		}
		store = registry.NewPostgresStore(pool)
	}

	handler := registry.NewHTTPHandler(
		registry.NewService(store, verifier, nil),
		authenticator,
	)
	address, err := localRegistryAddress(os.Getenv("AKUGC_REGISTRY_ADDR"))
	if err != nil {
		return err
	}
	server := &http.Server{
		Addr:              address,
		Handler:           handler,
		ReadHeaderTimeout: 5 * time.Second,
		ReadTimeout:       30 * time.Second,
		WriteTimeout:      120 * time.Second,
		IdleTimeout:       60 * time.Second,
		MaxHeaderBytes:    32 * 1024,
	}

	signals, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	go func() {
		<-signals.Done()
		shutdownContext, cancel := context.WithTimeout(context.Background(), 15*time.Second)
		defer cancel()
		_ = server.Shutdown(shutdownContext)
	}()

	log.Printf("Release Registry listening on %s", address)
	err = server.ListenAndServe()
	if errors.Is(err, http.ErrServerClosed) {
		return nil
	}
	return err
}

func localRegistryAddress(configured string) (string, error) {
	if configured == "" {
		return "127.0.0.1:8088", nil
	}
	host, _, err := net.SplitHostPort(configured)
	if err != nil {
		return "", errors.New("AKUGC_REGISTRY_ADDR must be a host:port address")
	}
	if strings.EqualFold(host, "localhost") {
		return configured, nil
	}
	ip := net.ParseIP(host)
	if ip == nil || !ip.IsLoopback() {
		return "", errors.New("AKUGC_REGISTRY_ADDR must use a loopback host in static-token local mode")
	}
	return configured, nil
}

func requiredFile(environmentName string) (string, error) {
	value := os.Getenv(environmentName)
	if value == "" {
		return "", errors.New(environmentName + " is required")
	}
	absolute, err := filepath.Abs(value)
	if err != nil {
		return "", errors.New(environmentName + " is invalid")
	}
	info, err := os.Stat(absolute)
	if err != nil || !info.Mode().IsRegular() {
		return "", errors.New(environmentName + " must name a regular file")
	}
	return absolute, nil
}
