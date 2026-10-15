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
	Submit(ctx context.Context, submission Submission) (release Release, created bool, err error)
	Get(ctx context.Context, releaseID string) (Release, error)
	Transition(ctx context.Context, request TransitionRequest) (release Release, changed bool, err error)
}
