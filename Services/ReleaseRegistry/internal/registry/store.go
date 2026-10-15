package registry

import (
	"context"
	"time"
)

type TransitionRequest struct {
	ReleaseID string
	Actor     string
	Target    Status
	Reason    string
	At        time.Time
}

type Store interface {
	CreateProject(ctx context.Context, creation ProjectCreation) (project Project, created bool, err error)
	GetProject(ctx context.Context, projectID string) (Project, error)
	GetProjectRole(ctx context.Context, projectID string, subject string) (role ProjectRole, member bool, err error)
	ListProjectMembers(ctx context.Context, projectID string) ([]ProjectMember, error)
	SetProjectMember(ctx context.Context, change ProjectMemberChange) (member ProjectMember, project Project, changed bool, err error)
	RemoveProjectMember(ctx context.Context, removal ProjectMemberRemoval) (project Project, changed bool, err error)
	ListProjectVersions(ctx context.Context, projectID string, cursor string, limit int) (ProjectVersionPage, error)
	Submit(ctx context.Context, submission Submission) (release Release, created bool, err error)
	Get(ctx context.Context, releaseID string) (Release, error)
	Transition(ctx context.Context, request TransitionRequest) (release Release, changed bool, err error)
}
