package registry

import (
	"errors"
	"slices"
	"time"
)

const MaxArtifactBytes = 16 * 1024 * 1024

type Role string

const (
	RoleCreator   Role = "creator"
	RoleReviewer  Role = "reviewer"
	RolePublisher Role = "publisher"
	RolePlayer    Role = "player"
)

type Principal struct {
	Subject       string
	Roles         []Role
	PublisherKeys []string
}

func (p Principal) HasRole(role Role) bool {
	return slices.Contains(p.Roles, role)
}

func (p Principal) AllowsPublisherKey(key string) bool {
	return slices.Contains(p.PublisherKeys, key)
}

type Status string

const (
	StatusSubmitted Status = "submitted"
	StatusApproved  Status = "approved"
	StatusRejected  Status = "rejected"
	StatusPublished Status = "published"
	StatusRevoked   Status = "revoked"
)

func (s Status) Valid() bool {
	switch s {
	case StatusSubmitted, StatusApproved, StatusRejected, StatusPublished, StatusRevoked:
		return true
	default:
		return false
	}
}

func canTransition(from, to Status) bool {
	switch {
	case from == StatusSubmitted && (to == StatusApproved || to == StatusRejected):
		return true
	case from == StatusApproved && to == StatusPublished:
		return true
	case from == StatusPublished && to == StatusRevoked:
		return true
	default:
		return false
	}
}

type PackMetadata struct {
	ReleaseID          string
	ProjectID          string
	SchemaVersion      int32
	ContentHash        string
	PublisherPublicKey string
	ArtifactSHA256     string
	ArtifactSize       int64
}

type ProjectRole string

const (
	ProjectRoleOwner  ProjectRole = "owner"
	ProjectRoleEditor ProjectRole = "editor"
	ProjectRoleViewer ProjectRole = "viewer"
)

func (r ProjectRole) ValidMemberRole() bool {
	return r == ProjectRoleEditor || r == ProjectRoleViewer
}

type ProjectVisibility string

const (
	ProjectVisibilityPrivate ProjectVisibility = "private"
	ProjectVisibilityPublic  ProjectVisibility = "public"
)

func (v ProjectVisibility) Valid() bool {
	return v == ProjectVisibilityPrivate || v == ProjectVisibilityPublic
}

type Project struct {
	ProjectID    string            `json:"projectId"`
	DisplayName  string            `json:"displayName"`
	OwnerSubject string            `json:"ownerSubject"`
	Visibility   ProjectVisibility `json:"visibility"`
	Revision     int64             `json:"revision"`
	CreatedAt    time.Time         `json:"createdAt"`
	UpdatedAt    time.Time         `json:"updatedAt"`
}

type ProjectMember struct {
	ProjectID string      `json:"projectId"`
	Subject   string      `json:"subject"`
	Role      ProjectRole `json:"role"`
	AddedAt   time.Time   `json:"addedAt"`
	UpdatedAt time.Time   `json:"updatedAt"`
}

type ProjectCreation struct {
	ProjectID      string
	DisplayName    string
	OwnerSubject   string
	IdempotencyKey string
	CreatedAt      time.Time
}

type ProjectMemberChange struct {
	ProjectID string
	Actor     string
	Subject   string
	Role      ProjectRole
	ChangedAt time.Time
}

type ProjectMemberRemoval struct {
	ProjectID string
	Actor     string
	Subject   string
	ChangedAt time.Time
}

type ProjectVisibilityChange struct {
	ProjectID  string
	Actor      string
	Visibility ProjectVisibility
	ChangedAt  time.Time
}

type ProjectVersionPage struct {
	Releases   []Release `json:"versions"`
	NextCursor string    `json:"nextCursor,omitempty"`
}

type Release struct {
	ReleaseID          string     `json:"releaseId"`
	ProjectID          string     `json:"projectId"`
	SchemaVersion      int32      `json:"schemaVersion"`
	OwnerSubject       string     `json:"ownerSubject"`
	SubmittedBySubject string     `json:"submittedBySubject"`
	PublisherPublicKey string     `json:"publisherPublicKey"`
	ContentHash        string     `json:"contentHash"`
	ArtifactSHA256     string     `json:"artifactSha256"`
	ArtifactSize       int64      `json:"artifactSize"`
	Status             Status     `json:"status"`
	Revision           int64      `json:"revision"`
	ReviewReason       string     `json:"reviewReason,omitempty"`
	SubmittedAt        time.Time  `json:"submittedAt"`
	ReviewedAt         *time.Time `json:"reviewedAt,omitempty"`
	PublishedAt        *time.Time `json:"publishedAt,omitempty"`
	RevokedAt          *time.Time `json:"revokedAt,omitempty"`
	UpdatedAt          time.Time  `json:"updatedAt"`
	Artifact           []byte     `json:"-"`
}

type BuildStatus string

const (
	BuildStatusQueued    BuildStatus = "queued"
	BuildStatusRunning   BuildStatus = "running"
	BuildStatusSucceeded BuildStatus = "succeeded"
	BuildStatusFailed    BuildStatus = "failed"
)

func (s BuildStatus) Valid() bool {
	switch s {
	case BuildStatusQueued, BuildStatusRunning, BuildStatusSucceeded, BuildStatusFailed:
		return true
	default:
		return false
	}
}

type BuildErrorCode string

const (
	BuildErrorValidationRejected   BuildErrorCode = "validation_rejected"
	BuildErrorValidatorUnavailable BuildErrorCode = "validator_unavailable"
	BuildErrorArtifactUnavailable  BuildErrorCode = "artifact_unavailable"
	BuildErrorArtifactCorrupt      BuildErrorCode = "artifact_corrupt"
	BuildErrorLeaseExpired         BuildErrorCode = "lease_expired"
)

func (c BuildErrorCode) Valid() bool {
	switch c {
	case BuildErrorValidationRejected,
		BuildErrorValidatorUnavailable,
		BuildErrorArtifactUnavailable,
		BuildErrorArtifactCorrupt,
		BuildErrorLeaseExpired:
		return true
	default:
		return false
	}
}

func (c BuildErrorCode) IsRetryable() bool {
	return c.Valid() && c != BuildErrorValidationRejected
}

type BuildJob struct {
	ReleaseID            string         `json:"releaseId"`
	ProjectID            string         `json:"projectId"`
	Status               BuildStatus    `json:"status"`
	Attempt              int            `json:"attempt"`
	Revision             int64          `json:"revision"`
	Retryable            bool           `json:"retryable"`
	InputArtifactSHA256  string         `json:"inputArtifactSha256"`
	OutputArtifactSHA256 string         `json:"outputArtifactSha256,omitempty"`
	OutputArtifactSize   int64          `json:"outputArtifactSize,omitempty"`
	ErrorCode            BuildErrorCode `json:"errorCode,omitempty"`
	CreatedAt            time.Time      `json:"createdAt"`
	StartedAt            *time.Time     `json:"startedAt,omitempty"`
	FinishedAt           *time.Time     `json:"finishedAt,omitempty"`
	UpdatedAt            time.Time      `json:"updatedAt"`
	leaseOwner           string
	leaseDeadline        *time.Time
}

func (b BuildJob) clone() BuildJob {
	if b.StartedAt != nil {
		value := *b.StartedAt
		b.StartedAt = &value
	}
	if b.FinishedAt != nil {
		value := *b.FinishedAt
		b.FinishedAt = &value
	}
	if b.leaseDeadline != nil {
		value := *b.leaseDeadline
		b.leaseDeadline = &value
	}
	return b
}

type BuildJobEvent struct {
	ReleaseID string
	Actor     string
	From      BuildStatus
	To        BuildStatus
	Attempt   int
	Revision  int64
	ErrorCode BuildErrorCode
	At        time.Time
}

type WorkPage struct {
	ProjectID     string            `json:"projectId"`
	DisplayName   string            `json:"displayName"`
	Visibility    ProjectVisibility `json:"visibility"`
	Revision      int64             `json:"revision"`
	UpdatedAt     time.Time         `json:"updatedAt"`
	LatestRelease Release           `json:"latestRelease"`
}

func (r Release) clone() Release {
	r.Artifact = append([]byte(nil), r.Artifact...)
	return r
}

type Submission struct {
	SubmitterSubject string
	IdempotencyKey   string
	Metadata         PackMetadata
	SubmittedAt      time.Time
}

type Transition struct {
	ReleaseID string
	Actor     string
	From      Status
	To        Status
	Reason    string
	At        time.Time
}

var (
	ErrUnauthenticated  = errors.New("unauthenticated")
	ErrForbidden        = errors.New("forbidden")
	ErrInvalidInput     = errors.New("invalid input")
	ErrNotFound         = errors.New("not found")
	ErrConflict         = errors.New("conflict")
	ErrUnavailable      = errors.New("unavailable")
	ErrNoBuildAvailable = errors.New("no build available")
)
