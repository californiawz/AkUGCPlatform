package registry

import (
	"context"
	"fmt"
	"sort"
	"sync"
	"time"
)

type idempotencyRecord struct {
	ArtifactSHA256 string
	ReleaseID      string
}

type projectIdempotencyRecord struct {
	ProjectID   string
	DisplayName string
}

type projectMemberEvent struct {
	ProjectID string
	Actor     string
	Subject   string
	FromRole  ProjectRole
	ToRole    ProjectRole
	At        time.Time
}

type projectVisibilityEvent struct {
	ProjectID      string
	Actor          string
	FromVisibility ProjectVisibility
	ToVisibility   ProjectVisibility
	At             time.Time
}

type MemoryStore struct {
	mutex              sync.RWMutex
	projects           map[string]Project
	projectMembers     map[string]map[string]ProjectMember
	projectIdempotency map[string]projectIdempotencyRecord
	memberEvents       []projectMemberEvent
	visibilityEvents   []projectVisibilityEvent
	releases           map[string]Release
	idempotency        map[string]idempotencyRecord
	transitions        []Transition
}

func NewMemoryStore() *MemoryStore {
	return &MemoryStore{
		projects:           make(map[string]Project),
		projectMembers:     make(map[string]map[string]ProjectMember),
		projectIdempotency: make(map[string]projectIdempotencyRecord),
		releases:           make(map[string]Release),
		idempotency:        make(map[string]idempotencyRecord),
	}
}

func (s *MemoryStore) CreateProject(
	_ context.Context,
	creation ProjectCreation,
) (Project, bool, error) {
	s.mutex.Lock()
	defer s.mutex.Unlock()

	idempotencyID := creation.OwnerSubject + "\x00" + creation.IdempotencyKey
	if previous, exists := s.projectIdempotency[idempotencyID]; exists {
		if previous.ProjectID != creation.ProjectID || previous.DisplayName != creation.DisplayName {
			return Project{}, false, fmt.Errorf("%w: idempotency key was reused with different project metadata", ErrConflict)
		}
		project, exists := s.projects[previous.ProjectID]
		if !exists {
			return Project{}, false, fmt.Errorf("%w: project idempotency record is inconsistent", ErrUnavailable)
		}
		return project, false, nil
	}

	if existing, exists := s.projects[creation.ProjectID]; exists {
		if existing.OwnerSubject != creation.OwnerSubject ||
			existing.DisplayName != creation.DisplayName {
			return Project{}, false, fmt.Errorf("%w: project ID already identifies different immutable metadata", ErrConflict)
		}
		s.projectIdempotency[idempotencyID] = projectIdempotencyRecord{
			ProjectID:   existing.ProjectID,
			DisplayName: existing.DisplayName,
		}
		return existing, false, nil
	}

	project := Project{
		ProjectID:    creation.ProjectID,
		DisplayName:  creation.DisplayName,
		OwnerSubject: creation.OwnerSubject,
		Visibility:   ProjectVisibilityPrivate,
		Revision:     1,
		CreatedAt:    creation.CreatedAt,
		UpdatedAt:    creation.CreatedAt,
	}
	s.projects[project.ProjectID] = project
	s.projectMembers[project.ProjectID] = make(map[string]ProjectMember)
	s.projectIdempotency[idempotencyID] = projectIdempotencyRecord{
		ProjectID:   project.ProjectID,
		DisplayName: project.DisplayName,
	}
	return project, true, nil
}

func (s *MemoryStore) GetProject(_ context.Context, projectID string) (Project, error) {
	s.mutex.RLock()
	defer s.mutex.RUnlock()
	project, exists := s.projects[projectID]
	if !exists {
		return Project{}, ErrNotFound
	}
	return project, nil
}

func (s *MemoryStore) GetProjectRole(
	_ context.Context,
	projectID string,
	subject string,
) (ProjectRole, bool, error) {
	s.mutex.RLock()
	defer s.mutex.RUnlock()
	project, exists := s.projects[projectID]
	if !exists {
		return "", false, ErrNotFound
	}
	if project.OwnerSubject == subject {
		return ProjectRoleOwner, true, nil
	}
	member, exists := s.projectMembers[projectID][subject]
	if !exists {
		return "", false, nil
	}
	return member.Role, true, nil
}

func (s *MemoryStore) ListProjectMembers(
	_ context.Context,
	projectID string,
) ([]ProjectMember, error) {
	s.mutex.RLock()
	defer s.mutex.RUnlock()
	project, exists := s.projects[projectID]
	if !exists {
		return nil, ErrNotFound
	}
	result := make([]ProjectMember, 0, len(s.projectMembers[projectID])+1)
	result = append(result, ProjectMember{
		ProjectID: project.ProjectID,
		Subject:   project.OwnerSubject,
		Role:      ProjectRoleOwner,
		AddedAt:   project.CreatedAt,
		UpdatedAt: project.CreatedAt,
	})
	for _, member := range s.projectMembers[projectID] {
		result = append(result, member)
	}
	sort.Slice(result[1:], func(left, right int) bool {
		return result[left+1].Subject < result[right+1].Subject
	})
	return result, nil
}

func (s *MemoryStore) SetProjectMember(
	_ context.Context,
	change ProjectMemberChange,
) (ProjectMember, Project, bool, error) {
	s.mutex.Lock()
	defer s.mutex.Unlock()
	project, exists := s.projects[change.ProjectID]
	if !exists || project.OwnerSubject != change.Actor {
		return ProjectMember{}, Project{}, false, ErrNotFound
	}
	if change.Subject == project.OwnerSubject {
		return ProjectMember{}, Project{}, false, fmt.Errorf("%w: project owner membership cannot be changed", ErrConflict)
	}
	members := s.projectMembers[change.ProjectID]
	existing, exists := members[change.Subject]
	if exists && existing.Role == change.Role {
		return existing, project, false, nil
	}
	member := ProjectMember{
		ProjectID: change.ProjectID,
		Subject:   change.Subject,
		Role:      change.Role,
		AddedAt:   change.ChangedAt,
		UpdatedAt: change.ChangedAt,
	}
	fromRole := ProjectRole("")
	if exists {
		member.AddedAt = existing.AddedAt
		fromRole = existing.Role
	}
	members[member.Subject] = member
	project.Revision++
	project.UpdatedAt = change.ChangedAt
	s.projects[project.ProjectID] = project
	s.memberEvents = append(s.memberEvents, projectMemberEvent{
		ProjectID: project.ProjectID,
		Actor:     change.Actor,
		Subject:   change.Subject,
		FromRole:  fromRole,
		ToRole:    change.Role,
		At:        change.ChangedAt,
	})
	return member, project, true, nil
}

func (s *MemoryStore) RemoveProjectMember(
	_ context.Context,
	removal ProjectMemberRemoval,
) (Project, bool, error) {
	s.mutex.Lock()
	defer s.mutex.Unlock()
	project, exists := s.projects[removal.ProjectID]
	if !exists || project.OwnerSubject != removal.Actor {
		return Project{}, false, ErrNotFound
	}
	if removal.Subject == project.OwnerSubject {
		return Project{}, false, fmt.Errorf("%w: project owner cannot be removed", ErrConflict)
	}
	member, exists := s.projectMembers[removal.ProjectID][removal.Subject]
	if !exists {
		return project, false, nil
	}
	delete(s.projectMembers[removal.ProjectID], removal.Subject)
	project.Revision++
	project.UpdatedAt = removal.ChangedAt
	s.projects[project.ProjectID] = project
	s.memberEvents = append(s.memberEvents, projectMemberEvent{
		ProjectID: project.ProjectID,
		Actor:     removal.Actor,
		Subject:   removal.Subject,
		FromRole:  member.Role,
		At:        removal.ChangedAt,
	})
	return project, true, nil
}

func (s *MemoryStore) SetProjectVisibility(
	_ context.Context,
	change ProjectVisibilityChange,
) (Project, bool, error) {
	s.mutex.Lock()
	defer s.mutex.Unlock()
	project, exists := s.projects[change.ProjectID]
	if !exists || project.OwnerSubject != change.Actor {
		return Project{}, false, ErrNotFound
	}
	if project.Visibility == change.Visibility {
		return project, false, nil
	}
	previous := project.Visibility
	project.Visibility = change.Visibility
	project.Revision++
	project.UpdatedAt = change.ChangedAt
	s.projects[project.ProjectID] = project
	s.visibilityEvents = append(s.visibilityEvents, projectVisibilityEvent{
		ProjectID:      project.ProjectID,
		Actor:          change.Actor,
		FromVisibility: previous,
		ToVisibility:   change.Visibility,
		At:             change.ChangedAt,
	})
	return project, true, nil
}

func (s *MemoryStore) ListProjectVersions(
	_ context.Context,
	projectID string,
	cursor string,
	limit int,
) (ProjectVersionPage, error) {
	s.mutex.RLock()
	defer s.mutex.RUnlock()
	if _, exists := s.projects[projectID]; !exists {
		return ProjectVersionPage{}, ErrNotFound
	}
	releases := make([]Release, 0)
	for _, release := range s.releases {
		if release.ProjectID == projectID {
			release.Artifact = nil
			releases = append(releases, release)
		}
	}
	sort.Slice(releases, func(left, right int) bool {
		if releases[left].SubmittedAt.Equal(releases[right].SubmittedAt) {
			return releases[left].ReleaseID > releases[right].ReleaseID
		}
		return releases[left].SubmittedAt.After(releases[right].SubmittedAt)
	})
	start := 0
	if cursor != "" {
		start = -1
		for index, release := range releases {
			if release.ReleaseID == cursor {
				start = index + 1
				break
			}
		}
		if start < 0 {
			return ProjectVersionPage{}, fmt.Errorf("%w: version cursor is not in this project", ErrInvalidInput)
		}
	}
	end := start + limit
	if end > len(releases) {
		end = len(releases)
	}
	page := ProjectVersionPage{
		Releases: append([]Release(nil), releases[start:end]...),
	}
	if end < len(releases) && len(page.Releases) > 0 {
		page.NextCursor = page.Releases[len(page.Releases)-1].ReleaseID
	}
	return page, nil
}

func (s *MemoryStore) GetLatestPublishedRelease(
	_ context.Context,
	projectID string,
) (Release, error) {
	s.mutex.RLock()
	defer s.mutex.RUnlock()
	if _, exists := s.projects[projectID]; !exists {
		return Release{}, ErrNotFound
	}
	var latest Release
	found := false
	for _, release := range s.releases {
		if release.ProjectID != projectID || release.Status != StatusPublished {
			continue
		}
		if !found ||
			release.SubmittedAt.After(latest.SubmittedAt) ||
			(release.SubmittedAt.Equal(latest.SubmittedAt) &&
				release.ReleaseID > latest.ReleaseID) {
			latest = release
			found = true
		}
	}
	if !found {
		return Release{}, ErrNotFound
	}
	return latest.clone(), nil
}

func (s *MemoryStore) Submit(
	_ context.Context,
	submission Submission,
) (Release, bool, error) {
	s.mutex.Lock()
	defer s.mutex.Unlock()

	project, exists := s.projects[submission.Metadata.ProjectID]
	if !exists {
		return Release{}, false, ErrNotFound
	}
	authorized := submission.SubmitterSubject == project.OwnerSubject
	if !authorized {
		member, memberExists := s.projectMembers[project.ProjectID][submission.SubmitterSubject]
		authorized = memberExists && member.Role == ProjectRoleEditor
	}
	if !authorized {
		return Release{}, false, ErrForbidden
	}

	idempotencyID := project.OwnerSubject + "\x00" + submission.IdempotencyKey
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
		if !sameImmutableRelease(existing, submission, project.OwnerSubject) {
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
		OwnerSubject:       project.OwnerSubject,
		SubmittedBySubject: submission.SubmitterSubject,
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

func sameImmutableRelease(
	existing Release,
	submission Submission,
	ownerSubject string,
) bool {
	metadata := submission.Metadata
	return existing.ReleaseID == metadata.ReleaseID &&
		existing.ProjectID == metadata.ProjectID &&
		existing.SchemaVersion == metadata.SchemaVersion &&
		existing.OwnerSubject == ownerSubject &&
		existing.PublisherPublicKey == metadata.PublisherPublicKey &&
		existing.ContentHash == metadata.ContentHash &&
		existing.ArtifactSHA256 == metadata.ArtifactSHA256 &&
		existing.ArtifactSize == metadata.ArtifactSize
}

func timePointer(value time.Time) *time.Time {
	return &value
}
