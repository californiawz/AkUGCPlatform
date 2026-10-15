package registry

import (
	"context"
	"fmt"
	"regexp"
	"strings"
	"time"
	"unicode"
	"unicode/utf8"

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

func (s *Service) CreateProject(
	ctx context.Context,
	principal Principal,
	idempotencyKey string,
	projectID string,
	displayName string,
) (Project, bool, error) {
	if principal.Subject == "" {
		return Project{}, false, ErrUnauthenticated
	}
	if !principal.HasRole(RoleCreator) {
		return Project{}, false, ErrForbidden
	}
	if !idempotencyKeyPattern.MatchString(idempotencyKey) {
		return Project{}, false, fmt.Errorf("%w: Idempotency-Key must be 16..128 safe ASCII characters", ErrInvalidInput)
	}
	canonicalID, err := canonicalUUID(projectID, true)
	if err != nil {
		return Project{}, false, fmt.Errorf("%w: invalid project ID", ErrInvalidInput)
	}
	if err := validateProjectDisplayName(displayName); err != nil {
		return Project{}, false, err
	}
	return s.store.CreateProject(ctx, ProjectCreation{
		ProjectID:      canonicalID,
		DisplayName:    displayName,
		OwnerSubject:   principal.Subject,
		IdempotencyKey: idempotencyKey,
		CreatedAt:      s.now(),
	})
}

func (s *Service) GetProject(
	ctx context.Context,
	principal Principal,
	projectID string,
) (Project, ProjectRole, error) {
	if principal.Subject == "" {
		return Project{}, "", ErrUnauthenticated
	}
	canonicalID, err := canonicalUUID(projectID, true)
	if err != nil {
		return Project{}, "", fmt.Errorf("%w: invalid project ID", ErrInvalidInput)
	}
	role, member, err := s.store.GetProjectRole(ctx, canonicalID, principal.Subject)
	if err != nil {
		return Project{}, "", err
	}
	if !member {
		return Project{}, "", ErrNotFound
	}
	project, err := s.store.GetProject(ctx, canonicalID)
	return project, role, err
}

func (s *Service) SetProjectVisibility(
	ctx context.Context,
	principal Principal,
	projectID string,
	visibility ProjectVisibility,
) (Project, bool, error) {
	if principal.Subject == "" {
		return Project{}, false, ErrUnauthenticated
	}
	canonicalID, err := canonicalUUID(projectID, true)
	if err != nil {
		return Project{}, false, fmt.Errorf("%w: invalid project ID", ErrInvalidInput)
	}
	if !visibility.Valid() {
		return Project{}, false, fmt.Errorf("%w: visibility must be private or public", ErrInvalidInput)
	}
	role, member, err := s.store.GetProjectRole(ctx, canonicalID, principal.Subject)
	if err != nil {
		return Project{}, false, err
	}
	if !member || role != ProjectRoleOwner {
		return Project{}, false, ErrNotFound
	}
	return s.store.SetProjectVisibility(ctx, ProjectVisibilityChange{
		ProjectID:  canonicalID,
		Actor:      principal.Subject,
		Visibility: visibility,
		ChangedAt:  s.now(),
	})
}

func (s *Service) GetWorkPage(
	ctx context.Context,
	principal Principal,
	projectID string,
) (WorkPage, error) {
	if principal.Subject == "" {
		return WorkPage{}, ErrUnauthenticated
	}
	if !principal.HasRole(RolePlayer) {
		return WorkPage{}, ErrForbidden
	}
	canonicalID, err := canonicalUUID(projectID, true)
	if err != nil {
		return WorkPage{}, fmt.Errorf("%w: invalid project ID", ErrInvalidInput)
	}
	project, err := s.store.GetProject(ctx, canonicalID)
	if err != nil {
		return WorkPage{}, err
	}
	if project.Visibility == ProjectVisibilityPrivate {
		_, member, err := s.store.GetProjectRole(ctx, canonicalID, principal.Subject)
		if err != nil {
			return WorkPage{}, err
		}
		if !member {
			return WorkPage{}, ErrNotFound
		}
	}
	release, err := s.store.GetLatestPublishedRelease(ctx, canonicalID)
	if err != nil {
		return WorkPage{}, err
	}
	release.Artifact = nil
	return WorkPage{
		ProjectID:     project.ProjectID,
		DisplayName:   project.DisplayName,
		Visibility:    project.Visibility,
		Revision:      project.Revision,
		UpdatedAt:     project.UpdatedAt,
		LatestRelease: release,
	}, nil
}

func (s *Service) ListProjectMembers(
	ctx context.Context,
	principal Principal,
	projectID string,
) ([]ProjectMember, error) {
	project, _, err := s.GetProject(ctx, principal, projectID)
	if err != nil {
		return nil, err
	}
	return s.store.ListProjectMembers(ctx, project.ProjectID)
}

func (s *Service) SetProjectMember(
	ctx context.Context,
	principal Principal,
	projectID string,
	subject string,
	role ProjectRole,
) (ProjectMember, Project, bool, error) {
	if principal.Subject == "" {
		return ProjectMember{}, Project{}, false, ErrUnauthenticated
	}
	canonicalID, err := canonicalUUID(projectID, true)
	if err != nil {
		return ProjectMember{}, Project{}, false, fmt.Errorf("%w: invalid project ID", ErrInvalidInput)
	}
	if !subjectPattern.MatchString(subject) {
		return ProjectMember{}, Project{}, false, fmt.Errorf("%w: invalid member subject", ErrInvalidInput)
	}
	if !role.ValidMemberRole() {
		return ProjectMember{}, Project{}, false, fmt.Errorf("%w: member role must be editor or viewer", ErrInvalidInput)
	}
	currentRole, member, err := s.store.GetProjectRole(ctx, canonicalID, principal.Subject)
	if err != nil {
		return ProjectMember{}, Project{}, false, err
	}
	if !member || currentRole != ProjectRoleOwner {
		return ProjectMember{}, Project{}, false, ErrNotFound
	}
	return s.store.SetProjectMember(ctx, ProjectMemberChange{
		ProjectID: canonicalID,
		Actor:     principal.Subject,
		Subject:   subject,
		Role:      role,
		ChangedAt: s.now(),
	})
}

func (s *Service) RemoveProjectMember(
	ctx context.Context,
	principal Principal,
	projectID string,
	subject string,
) (Project, bool, error) {
	if principal.Subject == "" {
		return Project{}, false, ErrUnauthenticated
	}
	canonicalID, err := canonicalUUID(projectID, true)
	if err != nil {
		return Project{}, false, fmt.Errorf("%w: invalid project ID", ErrInvalidInput)
	}
	if !subjectPattern.MatchString(subject) {
		return Project{}, false, fmt.Errorf("%w: invalid member subject", ErrInvalidInput)
	}
	currentRole, member, err := s.store.GetProjectRole(ctx, canonicalID, principal.Subject)
	if err != nil {
		return Project{}, false, err
	}
	if !member || currentRole != ProjectRoleOwner {
		return Project{}, false, ErrNotFound
	}
	return s.store.RemoveProjectMember(ctx, ProjectMemberRemoval{
		ProjectID: canonicalID,
		Actor:     principal.Subject,
		Subject:   subject,
		ChangedAt: s.now(),
	})
}

func (s *Service) ListProjectVersions(
	ctx context.Context,
	principal Principal,
	projectID string,
	cursor string,
	limit int,
) (ProjectVersionPage, error) {
	project, _, err := s.GetProject(ctx, principal, projectID)
	if err != nil {
		return ProjectVersionPage{}, err
	}
	canonicalCursor := ""
	if cursor != "" {
		canonicalCursor, err = canonicalUUID(cursor, true)
		if err != nil {
			return ProjectVersionPage{}, fmt.Errorf("%w: invalid version cursor", ErrInvalidInput)
		}
	}
	if limit < 1 || limit > 100 {
		return ProjectVersionPage{}, fmt.Errorf("%w: version limit must be 1..100", ErrInvalidInput)
	}
	return s.store.ListProjectVersions(ctx, project.ProjectID, canonicalCursor, limit)
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
	projectRole, member, err := s.store.GetProjectRole(
		ctx,
		envelope.ProjectID,
		principal.Subject,
	)
	if err != nil {
		return Release{}, false, err
	}
	if !member || (projectRole != ProjectRoleOwner && projectRole != ProjectRoleEditor) {
		return Release{}, false, fmt.Errorf("%w: creator cannot submit to this project", ErrForbidden)
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
		SubmitterSubject: principal.Subject,
		IdempotencyKey:   idempotencyKey,
		Metadata:         metadata,
		Artifact:         append([]byte(nil), artifact...),
		SubmittedAt:      s.now(),
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
	if _, member, projectErr := s.store.GetProjectRole(
		ctx,
		current.ProjectID,
		principal.Subject,
	); projectErr != nil {
		return Release{}, false, projectErr
	} else if member {
		return Release{}, false, fmt.Errorf("%w: project members cannot review their project", ErrForbidden)
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
		release.OwnerSubject == principal.Subject {
		release.Artifact = nil
		return release, nil
	}
	if _, member, projectErr := s.store.GetProjectRole(
		ctx,
		release.ProjectID,
		principal.Subject,
	); projectErr != nil {
		return Release{}, projectErr
	} else if member {
		release.Artifact = nil
		return release, nil
	}
	if release.Status == StatusPublished && principal.HasRole(RolePlayer) {
		project, projectErr := s.store.GetProject(ctx, release.ProjectID)
		if projectErr != nil {
			return Release{}, projectErr
		}
		if project.Visibility == ProjectVisibilityPublic {
			release.Artifact = nil
			return release, nil
		}
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
	project, err := s.store.GetProject(ctx, release.ProjectID)
	if err != nil {
		return Release{}, err
	}
	if project.Visibility != ProjectVisibilityPublic {
		_, member, err := s.store.GetProjectRole(
			ctx,
			release.ProjectID,
			principal.Subject,
		)
		if err != nil {
			return Release{}, err
		}
		if !member {
			return Release{}, ErrNotFound
		}
	}
	return release, nil
}

func validateProjectDisplayName(value string) error {
	if !utf8.ValidString(value) {
		return fmt.Errorf("%w: project display name must be valid UTF-8", ErrInvalidInput)
	}
	if len([]byte(value)) < 1 || len([]byte(value)) > 128 {
		return fmt.Errorf("%w: project display name must be 1..128 UTF-8 bytes", ErrInvalidInput)
	}
	if strings.TrimSpace(value) != value {
		return fmt.Errorf("%w: project display name cannot have leading or trailing whitespace", ErrInvalidInput)
	}
	for _, character := range value {
		if unicode.IsControl(character) {
			return fmt.Errorf("%w: project display name cannot contain control characters", ErrInvalidInput)
		}
	}
	return nil
}
