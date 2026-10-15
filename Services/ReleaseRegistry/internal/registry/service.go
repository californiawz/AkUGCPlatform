package registry

import (
	"context"
	"fmt"
	"regexp"
	"strings"
	"time"

	"golang.org/x/sync/singleflight"
)

var idempotencyKeyPattern = regexp.MustCompile(`^[A-Za-z0-9._:-]{16,128}$`)

type Service struct {
	store       Store
	verifier    PackVerifier
	now         func() time.Time
	validations singleflight.Group
}

func NewService(store Store, verifier PackVerifier, now func() time.Time) *Service {
	if now == nil {
		now = func() time.Time { return time.Now().UTC() }
	}
	return &Service{store: store, verifier: verifier, now: now}
}

func (s *Service) Submit(
	ctx context.Context,
	principal Principal,
	idempotencyKey string,
	artifact []byte,
) (Release, bool, error) {
	if principal.Subject == "" {
		return Release{}, false, ErrUnauthenticated
	}
	if !principal.HasRole(RoleCreator) {
		return Release{}, false, ErrForbidden
	}
	if !idempotencyKeyPattern.MatchString(idempotencyKey) {
		return Release{}, false, fmt.Errorf("%w: Idempotency-Key must be 16..128 safe ASCII characters", ErrInvalidInput)
	}
	envelope, err := InspectAndVerifyEnvelope(artifact)
	if err != nil {
		return Release{}, false, err
	}
	if !principal.AllowsPublisherKey(envelope.PublisherPublicKey) {
		return Release{}, false, fmt.Errorf("%w: publisher key is not registered to this creator", ErrForbidden)
	}
	if s.verifier == nil {
		return Release{}, false, fmt.Errorf("%w: pack verifier is not configured", ErrUnavailable)
	}
	validationKey := principal.Subject + "\n" + idempotencyKey + "\n" + envelope.ArtifactSHA256
	verified, err, _ := s.validations.Do(validationKey, func() (any, error) {
		return s.verifier.Verify(ctx, artifact, envelope.PublisherPublicKey)
	})
	if err != nil {
		return Release{}, false, err
	}
	metadata, ok := verified.(PackMetadata)
	if !ok {
		return Release{}, false, fmt.Errorf("%w: pack verifier returned an invalid result", ErrUnavailable)
	}
	if metadata != envelope {
		return Release{}, false, fmt.Errorf("%w: verifier metadata mismatch", ErrInvalidInput)
	}
	return s.store.Submit(ctx, Submission{
		OwnerSubject:   principal.Subject,
		IdempotencyKey: idempotencyKey,
		Metadata:       metadata,
		Artifact:       append([]byte(nil), artifact...),
		SubmittedAt:    s.now(),
	})
}

func (s *Service) Review(
	ctx context.Context,
	principal Principal,
	releaseID string,
	decision Status,
	reason string,
) (Release, bool, error) {
	if principal.Subject == "" {
		return Release{}, false, ErrUnauthenticated
	}
	if !principal.HasRole(RoleReviewer) {
		return Release{}, false, ErrForbidden
	}
	if decision != StatusApproved && decision != StatusRejected {
		return Release{}, false, fmt.Errorf("%w: review decision must be approved or rejected", ErrInvalidInput)
	}
	reason = strings.TrimSpace(reason)
	if len([]byte(reason)) > 1024 || (decision == StatusRejected && reason == "") {
		return Release{}, false, fmt.Errorf("%w: rejection requires a reason of at most 1024 bytes", ErrInvalidInput)
	}
	canonicalID, err := canonicalUUID(releaseID, true)
	if err != nil {
		return Release{}, false, fmt.Errorf("%w: invalid release ID", ErrInvalidInput)
	}
	current, err := s.store.Get(ctx, canonicalID)
	if err != nil {
		return Release{}, false, err
	}
	if current.OwnerSubject == principal.Subject {
		return Release{}, false, fmt.Errorf("%w: creator cannot review their own release", ErrForbidden)
	}
	return s.store.Transition(ctx, TransitionRequest{
		ReleaseID: canonicalID,
		Actor:     principal.Subject,
		Target:    decision,
		Reason:    reason,
		At:        s.now(),
	})
}

func (s *Service) Publish(
	ctx context.Context,
	principal Principal,
	releaseID string,
) (Release, bool, error) {
	return s.ownerTransition(
		ctx,
		principal,
		releaseID,
		RolePublisher,
		StatusPublished,
		"",
	)
}

func (s *Service) Revoke(
	ctx context.Context,
	principal Principal,
	releaseID string,
	reason string,
) (Release, bool, error) {
	reason = strings.TrimSpace(reason)
	if reason == "" || len([]byte(reason)) > 1024 {
		return Release{}, false, fmt.Errorf("%w: revocation requires a reason of at most 1024 bytes", ErrInvalidInput)
	}
	return s.ownerTransition(
		ctx,
		principal,
		releaseID,
		RolePublisher,
		StatusRevoked,
		reason,
	)
}

func (s *Service) ownerTransition(
	ctx context.Context,
	principal Principal,
	releaseID string,
	role Role,
	target Status,
	reason string,
) (Release, bool, error) {
	if principal.Subject == "" {
		return Release{}, false, ErrUnauthenticated
	}
	if !principal.HasRole(role) {
		return Release{}, false, ErrForbidden
	}
	canonicalID, err := canonicalUUID(releaseID, true)
	if err != nil {
		return Release{}, false, fmt.Errorf("%w: invalid release ID", ErrInvalidInput)
	}
	current, err := s.store.Get(ctx, canonicalID)
	if err != nil {
		return Release{}, false, err
	}
	if current.OwnerSubject != principal.Subject {
		return Release{}, false, ErrForbidden
	}
	return s.store.Transition(ctx, TransitionRequest{
		ReleaseID: canonicalID,
		Actor:     principal.Subject,
		Target:    target,
		Reason:    reason,
		At:        s.now(),
	})
}

func (s *Service) Get(
	ctx context.Context,
	principal Principal,
	releaseID string,
) (Release, error) {
	if principal.Subject == "" {
		return Release{}, ErrUnauthenticated
	}
	canonicalID, err := canonicalUUID(releaseID, true)
	if err != nil {
		return Release{}, fmt.Errorf("%w: invalid release ID", ErrInvalidInput)
	}
	release, err := s.store.Get(ctx, canonicalID)
	if err != nil {
		return Release{}, err
	}
	if principal.HasRole(RoleReviewer) ||
		release.OwnerSubject == principal.Subject ||
		(release.Status == StatusPublished && principal.HasRole(RolePlayer)) {
		release.Artifact = nil
		return release, nil
	}
	return Release{}, ErrNotFound
}

func (s *Service) Download(
	ctx context.Context,
	principal Principal,
	releaseID string,
) (Release, error) {
	if principal.Subject == "" {
		return Release{}, ErrUnauthenticated
	}
	if !principal.HasRole(RolePlayer) {
		return Release{}, ErrForbidden
	}
	canonicalID, err := canonicalUUID(releaseID, true)
	if err != nil {
		return Release{}, fmt.Errorf("%w: invalid release ID", ErrInvalidInput)
	}
	release, err := s.store.Get(ctx, canonicalID)
	if err != nil {
		return Release{}, err
	}
	if release.Status != StatusPublished {
		return Release{}, ErrNotFound
	}
	return release, nil
}
