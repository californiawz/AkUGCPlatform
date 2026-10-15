package registry

import (
	"context"
	"time"
)

const MaxAutomaticBuildAttempts = 3

type TransitionRequest struct {
	ReleaseID string
	Actor     string
	Target    Status
	Reason    string
	At        time.Time
}

type ClaimBuildRequest struct {
	Worker        string
	At            time.Time
	LeaseDuration time.Duration
}

type CompleteBuildRequest struct {
	ReleaseID            string
	Worker               string
	Succeeded            bool
	OutputArtifactSHA256 string
	OutputArtifactSize   int64
	ErrorCode            BuildErrorCode
	At                   time.Time
}

type RetryBuildRequest struct {
	ReleaseID string
	Actor     string
	At        time.Time
}

type Store interface {
	CreateProject(ctx context.Context, creation ProjectCreation) (project Project, created bool, err error)
	GetProject(ctx context.Context, projectID string) (Project, error)
	GetProjectRole(ctx context.Context, projectID string, subject string) (role ProjectRole, member bool, err error)
	ListProjectMembers(ctx context.Context, projectID string) ([]ProjectMember, error)
	SetProjectMember(ctx context.Context, change ProjectMemberChange) (member ProjectMember, project Project, changed bool, err error)
	RemoveProjectMember(ctx context.Context, removal ProjectMemberRemoval) (project Project, changed bool, err error)
	SetProjectVisibility(ctx context.Context, change ProjectVisibilityChange) (project Project, changed bool, err error)
	ListProjectVersions(ctx context.Context, projectID string, cursor string, limit int) (ProjectVersionPage, error)
	GetLatestPublishedRelease(ctx context.Context, projectID string) (Release, error)
	Submit(ctx context.Context, submission Submission) (release Release, build BuildJob, created bool, err error)
	Get(ctx context.Context, releaseID string) (Release, error)
	Transition(ctx context.Context, request TransitionRequest) (release Release, changed bool, err error)
	GetBuild(ctx context.Context, releaseID string) (BuildJob, error)
	ClaimBuild(ctx context.Context, request ClaimBuildRequest) (BuildJob, error)
	CompleteBuild(ctx context.Context, request CompleteBuildRequest) (build BuildJob, changed bool, err error)
	RetryBuild(ctx context.Context, request RetryBuildRequest) (build BuildJob, changed bool, err error)
}
