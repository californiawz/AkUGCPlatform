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

type Release struct {
	ReleaseID          string     `json:"releaseId"`
	ProjectID          string     `json:"projectId"`
	SchemaVersion      int32      `json:"schemaVersion"`
	OwnerSubject       string     `json:"ownerSubject"`
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

func (r Release) clone() Release {
	r.Artifact = append([]byte(nil), r.Artifact...)
	return r
}

type Submission struct {
	OwnerSubject   string
	IdempotencyKey string
	Metadata       PackMetadata
	Artifact       []byte
	SubmittedAt    time.Time
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
	ErrUnauthenticated = errors.New("unauthenticated")
	ErrForbidden       = errors.New("forbidden")
	ErrInvalidInput    = errors.New("invalid input")
	ErrNotFound        = errors.New("not found")
	ErrConflict        = errors.New("conflict")
	ErrUnavailable     = errors.New("unavailable")
)
