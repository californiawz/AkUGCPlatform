package registry

import (
	"context"
	"fmt"
	"sync"
	"time"
)

type idempotencyRecord struct {
	ArtifactSHA256 string
	ReleaseID      string
}

type MemoryStore struct {
	mutex       sync.RWMutex
	releases    map[string]Release
	idempotency map[string]idempotencyRecord
	transitions []Transition
}

func NewMemoryStore() *MemoryStore {
	return &MemoryStore{
		releases:    make(map[string]Release),
		idempotency: make(map[string]idempotencyRecord),
	}
}

func (s *MemoryStore) Submit(
	_ context.Context,
	submission Submission,
) (Release, bool, error) {
	s.mutex.Lock()
	defer s.mutex.Unlock()

	idempotencyID := submission.OwnerSubject + "\x00" + submission.IdempotencyKey
	if previous, exists := s.idempotency[idempotencyID]; exists {
		if previous.ArtifactSHA256 != submission.Metadata.ArtifactSHA256 {
			return Release{}, false, fmt.Errorf("%w: idempotency key was reused with different content", ErrConflict)
		}
		release, exists := s.releases[previous.ReleaseID]
		if !exists {
			return Release{}, false, fmt.Errorf("%w: idempotency record is inconsistent", ErrUnavailable)
		}
		return release.clone(), false, nil
	}

	if existing, exists := s.releases[submission.Metadata.ReleaseID]; exists {
		if !sameImmutableRelease(existing, submission) {
			return Release{}, false, fmt.Errorf("%w: release ID already identifies different immutable content", ErrConflict)
		}
		s.idempotency[idempotencyID] = idempotencyRecord{
			ArtifactSHA256: submission.Metadata.ArtifactSHA256,
			ReleaseID:      existing.ReleaseID,
		}
		return existing.clone(), false, nil
	}

	release := Release{
		ReleaseID:          submission.Metadata.ReleaseID,
		ProjectID:          submission.Metadata.ProjectID,
		SchemaVersion:      submission.Metadata.SchemaVersion,
		OwnerSubject:       submission.OwnerSubject,
		PublisherPublicKey: submission.Metadata.PublisherPublicKey,
		ContentHash:        submission.Metadata.ContentHash,
		ArtifactSHA256:     submission.Metadata.ArtifactSHA256,
		ArtifactSize:       submission.Metadata.ArtifactSize,
		Status:             StatusSubmitted,
		Revision:           1,
		SubmittedAt:        submission.SubmittedAt,
		UpdatedAt:          submission.SubmittedAt,
		Artifact:           append([]byte(nil), submission.Artifact...),
	}
	s.releases[release.ReleaseID] = release
	s.idempotency[idempotencyID] = idempotencyRecord{
		ArtifactSHA256: release.ArtifactSHA256,
		ReleaseID:      release.ReleaseID,
	}
	return release.clone(), true, nil
}

func (s *MemoryStore) Get(_ context.Context, releaseID string) (Release, error) {
	s.mutex.RLock()
	defer s.mutex.RUnlock()
	release, exists := s.releases[releaseID]
	if !exists {
		return Release{}, ErrNotFound
	}
	return release.clone(), nil
}

func (s *MemoryStore) Transition(
	_ context.Context,
	request TransitionRequest,
) (Release, bool, error) {
	if !request.Target.Valid() {
		return Release{}, false, fmt.Errorf("%w: invalid target status", ErrInvalidInput)
	}
	s.mutex.Lock()
	defer s.mutex.Unlock()

	release, exists := s.releases[request.ReleaseID]
	if !exists {
		return Release{}, false, ErrNotFound
	}
	if release.Status == request.Target {
		return release.clone(), false, nil
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
	s.releases[release.ReleaseID] = release
	s.transitions = append(s.transitions, Transition{
		ReleaseID: release.ReleaseID,
		Actor:     request.Actor,
		From:      previous,
		To:        request.Target,
		Reason:    request.Reason,
		At:        request.At,
	})
	return release.clone(), true, nil
}

func sameImmutableRelease(existing Release, submission Submission) bool {
	metadata := submission.Metadata
	return existing.ReleaseID == metadata.ReleaseID &&
		existing.ProjectID == metadata.ProjectID &&
		existing.SchemaVersion == metadata.SchemaVersion &&
		existing.OwnerSubject == submission.OwnerSubject &&
		existing.PublisherPublicKey == metadata.PublisherPublicKey &&
		existing.ContentHash == metadata.ContentHash &&
		existing.ArtifactSHA256 == metadata.ArtifactSHA256 &&
		existing.ArtifactSize == metadata.ArtifactSize
}

func timePointer(value time.Time) *time.Time {
	return &value
}
