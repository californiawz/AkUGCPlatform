package registry

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"sync"
	"sync/atomic"
	"testing"
	"time"
)

type acceptingVerifier struct{}

func (acceptingVerifier) Verify(
	_ context.Context,
	artifact []byte,
	trustedKey string,
) (PackMetadata, error) {
	metadata, err := InspectAndVerifyEnvelope(artifact)
	if err != nil {
		return PackMetadata{}, err
	}
	if metadata.PublisherPublicKey != trustedKey {
		return PackMetadata{}, ErrForbidden
	}
	return metadata, nil
}

type countingVerifier struct {
	calls atomic.Int32
}

func (v *countingVerifier) Verify(
	ctx context.Context,
	artifact []byte,
	trustedKey string,
) (PackMetadata, error) {
	v.calls.Add(1)
	time.Sleep(25 * time.Millisecond)
	return acceptingVerifier{}.Verify(ctx, artifact, trustedKey)
}

func TestReleaseLifecyclePermissionsAndImmutability(t *testing.T) {
	artifact, _, publicKey := signedArtifact(t, "31111111-2222-3333-4444-555555555555")
	now := time.Date(2026, 10, 15, 8, 0, 0, 0, time.UTC)
	service := NewService(NewMemoryStore(), acceptingVerifier{}, func() time.Time {
		now = now.Add(time.Second)
		return now
	})
	creator := Principal{
		Subject:       "creator-a",
		Roles:         []Role{RoleCreator, RolePublisher},
		PublisherKeys: []string{publicKey},
	}
	reviewer := Principal{Subject: "reviewer", Roles: []Role{RoleReviewer}}
	player := Principal{Subject: "player-b", Roles: []Role{RolePlayer}}
	project := createServiceTestProject(t, service, creator)
	if project.Visibility != ProjectVisibilityPrivate {
		t.Fatalf("new project visibility = %q", project.Visibility)
	}

	release, created, err := service.Submit(
		context.Background(),
		creator,
		"submit-31111111-v1",
		artifact,
	)
	if err != nil || !created || release.Status != StatusSubmitted {
		t.Fatalf("submit failed: created=%v release=%+v err=%v", created, release, err)
	}
	retried, created, err := service.Submit(
		context.Background(),
		creator,
		"submit-31111111-v1",
		artifact,
	)
	if err != nil || created || retried.ReleaseID != release.ReleaseID {
		t.Fatalf("idempotent retry failed: created=%v release=%+v err=%v", created, retried, err)
	}

	if _, _, err := service.Publish(context.Background(), creator, release.ReleaseID); !errors.Is(err, ErrConflict) {
		t.Fatalf("unapproved publish should conflict: %v", err)
	}
	selfReviewer := creator
	selfReviewer.Roles = append(selfReviewer.Roles, RoleReviewer)
	if _, _, err := service.Review(
		context.Background(),
		selfReviewer,
		release.ReleaseID,
		StatusApproved,
		"",
	); !errors.Is(err, ErrForbidden) {
		t.Fatalf("self review should be forbidden: %v", err)
	}
	approved, changed, err := service.Review(
		context.Background(),
		reviewer,
		release.ReleaseID,
		StatusApproved,
		"fixture approved",
	)
	if err != nil || !changed || approved.Status != StatusApproved {
		t.Fatalf("review failed: changed=%v release=%+v err=%v", changed, approved, err)
	}
	if _, _, err := service.Review(
		context.Background(),
		reviewer,
		release.ReleaseID,
		StatusRejected,
		"late rejection",
	); !errors.Is(err, ErrConflict) {
		t.Fatalf("conflicting review should fail: %v", err)
	}
	published, changed, err := service.Publish(context.Background(), creator, release.ReleaseID)
	if err != nil || !changed || published.Status != StatusPublished {
		t.Fatalf("publish failed: changed=%v release=%+v err=%v", changed, published, err)
	}
	if _, err := service.Download(context.Background(), player, release.ReleaseID); !errors.Is(err, ErrNotFound) {
		t.Fatalf("private project download should be hidden: %v", err)
	}
	if _, _, err := service.SetProjectVisibility(
		context.Background(),
		player,
		project.ProjectID,
		ProjectVisibilityPublic,
	); !errors.Is(err, ErrNotFound) {
		t.Fatalf("non-owner visibility change should be hidden: %v", err)
	}
	publicProject, changed, err := service.SetProjectVisibility(
		context.Background(),
		creator,
		project.ProjectID,
		ProjectVisibilityPublic,
	)
	if err != nil || !changed ||
		publicProject.Visibility != ProjectVisibilityPublic ||
		publicProject.Revision != project.Revision+1 {
		t.Fatalf("public visibility failed: changed=%v project=%+v err=%v", changed, publicProject, err)
	}
	retriedProject, changed, err := service.SetProjectVisibility(
		context.Background(),
		creator,
		project.ProjectID,
		ProjectVisibilityPublic,
	)
	if err != nil || changed || retriedProject.Revision != publicProject.Revision {
		t.Fatalf("visibility retry was not idempotent: changed=%v project=%+v err=%v", changed, retriedProject, err)
	}
	work, err := service.GetWorkPage(context.Background(), player, project.ProjectID)
	if err != nil ||
		work.Visibility != ProjectVisibilityPublic ||
		work.LatestRelease.ReleaseID != release.ReleaseID ||
		work.LatestRelease.Artifact != nil {
		t.Fatalf("public work page mismatch: work=%+v err=%v", work, err)
	}
	downloaded, err := service.Download(context.Background(), player, release.ReleaseID)
	if err != nil || !bytes.Equal(downloaded.Artifact, artifact) {
		t.Fatalf("published download failed: release=%+v err=%v", downloaded, err)
	}

	collision := append(append([]byte(nil), artifact...), '\n')
	if _, _, err := service.Submit(
		context.Background(),
		creator,
		"submit-collision-v1",
		collision,
	); !errors.Is(err, ErrConflict) {
		t.Fatalf("release ID collision should fail: %v", err)
	}
	revoked, changed, err := service.Revoke(
		context.Background(),
		creator,
		release.ReleaseID,
		"security withdrawal",
	)
	if err != nil || !changed || revoked.Status != StatusRevoked {
		t.Fatalf("revoke failed: changed=%v release=%+v err=%v", changed, revoked, err)
	}
	if _, err := service.Download(context.Background(), player, release.ReleaseID); !errors.Is(err, ErrNotFound) {
		t.Fatalf("revoked download should be hidden: %v", err)
	}
	if _, err := service.GetWorkPage(context.Background(), player, project.ProjectID); !errors.Is(err, ErrNotFound) {
		t.Fatalf("work page without a published release should be hidden: %v", err)
	}
}

func TestConcurrentSubmissionRetryCreatesOneRelease(t *testing.T) {
	artifact, _, publicKey := signedArtifact(t, "41111111-2222-3333-4444-555555555555")
	verifier := &countingVerifier{}
	service := NewService(NewMemoryStore(), verifier, func() time.Time {
		return time.Date(2026, 10, 15, 9, 0, 0, 0, time.UTC)
	})
	creator := Principal{
		Subject:       "creator-a",
		Roles:         []Role{RoleCreator},
		PublisherKeys: []string{publicKey},
	}
	createServiceTestProject(t, service, creator)

	var createdCount atomic.Int32
	var wait sync.WaitGroup
	errorsFound := make(chan error, 32)
	for index := 0; index < 32; index++ {
		wait.Add(1)
		go func() {
			defer wait.Done()
			_, created, err := service.Submit(
				context.Background(),
				creator,
				"concurrent-submit-41111111",
				artifact,
			)
			if err != nil {
				errorsFound <- err
				return
			}
			if created {
				createdCount.Add(1)
			}
		}()
	}
	wait.Wait()
	close(errorsFound)
	for err := range errorsFound {
		t.Errorf("concurrent submit failed: %v", err)
	}
	if createdCount.Load() != 1 {
		t.Fatalf("expected exactly one creation, got %d", createdCount.Load())
	}
	if verifier.calls.Load() != 1 {
		t.Fatalf("expected one shared deep validation, got %d", verifier.calls.Load())
	}
}

func TestConcurrentVisibilityRetryChangesOnce(t *testing.T) {
	store := NewMemoryStore()
	service := NewService(store, acceptingVerifier{}, func() time.Time {
		return time.Date(2026, 10, 15, 10, 0, 0, 0, time.UTC)
	})
	owner := Principal{
		Subject: "creator-a",
		Roles:   []Role{RoleCreator},
	}
	project := createServiceTestProject(t, service, owner)
	var changedCount atomic.Int32
	var wait sync.WaitGroup
	failures := make(chan error, 32)
	for range 32 {
		wait.Add(1)
		go func() {
			defer wait.Done()
			visible, changed, err := service.SetProjectVisibility(
				context.Background(),
				owner,
				project.ProjectID,
				ProjectVisibilityPublic,
			)
			if err != nil {
				failures <- err
				return
			}
			if visible.Visibility != ProjectVisibilityPublic {
				failures <- fmt.Errorf("visibility = %q", visible.Visibility)
			}
			if changed {
				changedCount.Add(1)
			}
		}()
	}
	wait.Wait()
	close(failures)
	for err := range failures {
		t.Errorf("visibility retry failed: %v", err)
	}
	if changedCount.Load() != 1 {
		t.Fatalf("visibility changed count = %d", changedCount.Load())
	}
	stored, _, err := service.GetProject(context.Background(), owner, project.ProjectID)
	if err != nil || stored.Revision != 2 || len(store.visibilityEvents) != 1 {
		t.Fatalf(
			"visibility final state mismatch: project=%+v events=%d err=%v",
			stored,
			len(store.visibilityEvents),
			err,
		)
	}
}

func TestIdempotencyKeyCannotBindDifferentArtifact(t *testing.T) {
	first, _, publicKey := signedArtifact(t, "51111111-2222-3333-4444-555555555555")
	second, _, _ := signedArtifact(t, "61111111-2222-3333-4444-555555555555")
	service := NewService(NewMemoryStore(), acceptingVerifier{}, nil)
	creator := Principal{
		Subject:       "creator-a",
		Roles:         []Role{RoleCreator},
		PublisherKeys: []string{publicKey},
	}
	createServiceTestProject(t, service, creator)
	if _, _, err := service.Submit(
		context.Background(), creator, "same-key-different-v1", first,
	); err != nil {
		t.Fatal(err)
	}
	if _, _, err := service.Submit(
		context.Background(), creator, "same-key-different-v1", second,
	); !errors.Is(err, ErrConflict) {
		t.Fatalf("different artifact reused idempotency key: %v", err)
	}
}

func TestProjectCollaborationAndVersionPermissions(t *testing.T) {
	store := NewMemoryStore()
	now := time.Date(2026, 10, 15, 13, 0, 0, 0, time.UTC)
	service := NewService(store, acceptingVerifier{}, func() time.Time {
		now = now.Add(time.Second)
		return now
	})
	first, _, publicKey := signedArtifact(t, "d1111111-2222-3333-4444-555555555555")
	second, _, _ := signedArtifact(t, "e1111111-2222-3333-4444-555555555555")
	owner := Principal{
		Subject:       "creator-a",
		Roles:         []Role{RoleCreator, RolePublisher},
		PublisherKeys: []string{publicKey},
	}
	editor := Principal{
		Subject:       "creator-editor",
		Roles:         []Role{RoleCreator, RolePublisher, RoleReviewer},
		PublisherKeys: []string{publicKey},
	}
	viewer := Principal{Subject: "project-viewer", Roles: []Role{RolePlayer}}
	outsider := Principal{Subject: "project-outsider", Roles: []Role{RolePlayer}}
	reviewer := Principal{Subject: "independent-reviewer", Roles: []Role{RoleReviewer}}

	project, created, err := service.CreateProject(
		context.Background(),
		owner,
		"create-project-collaboration-v1",
		"aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee",
		"Collaboration fixture",
	)
	if err != nil || !created || project.OwnerSubject != owner.Subject {
		t.Fatalf("create project failed: created=%v project=%+v err=%v", created, project, err)
	}
	retried, created, err := service.CreateProject(
		context.Background(),
		owner,
		"create-project-collaboration-v1",
		project.ProjectID,
		project.DisplayName,
	)
	if err != nil || created || retried != project {
		t.Fatalf("project retry failed: created=%v project=%+v err=%v", created, retried, err)
	}
	if _, _, err := service.CreateProject(
		context.Background(),
		owner,
		"create-project-collaboration-v1",
		project.ProjectID,
		"Different name",
	); !errors.Is(err, ErrConflict) {
		t.Fatalf("project idempotency conflict was not rejected: %v", err)
	}
	if _, _, _, err := service.SetProjectMember(
		context.Background(),
		owner,
		project.ProjectID,
		editor.Subject,
		ProjectRoleEditor,
	); err != nil {
		t.Fatal(err)
	}
	if _, _, _, err := service.SetProjectMember(
		context.Background(),
		owner,
		project.ProjectID,
		viewer.Subject,
		ProjectRoleViewer,
	); err != nil {
		t.Fatal(err)
	}
	if _, _, _, err := service.SetProjectMember(
		context.Background(),
		editor,
		project.ProjectID,
		outsider.Subject,
		ProjectRoleViewer,
	); !errors.Is(err, ErrNotFound) {
		t.Fatalf("non-owner member mutation should be hidden: %v", err)
	}

	firstRelease, created, err := service.Submit(
		context.Background(),
		editor,
		"editor-submit-first-v1",
		first,
	)
	if err != nil || !created {
		t.Fatalf("editor submit failed: created=%v release=%+v err=%v", created, firstRelease, err)
	}
	if firstRelease.OwnerSubject != owner.Subject ||
		firstRelease.SubmittedBySubject != editor.Subject {
		t.Fatalf("release ownership attribution mismatch: %+v", firstRelease)
	}
	if _, _, err := service.Review(
		context.Background(),
		editor,
		firstRelease.ReleaseID,
		StatusApproved,
		"",
	); !errors.Is(err, ErrForbidden) {
		t.Fatalf("project member self-review should be forbidden: %v", err)
	}
	viewerReviewer := viewer
	viewerReviewer.Roles = []Role{RoleReviewer}
	if _, _, err := service.Review(
		context.Background(),
		viewerReviewer,
		firstRelease.ReleaseID,
		StatusApproved,
		"",
	); !errors.Is(err, ErrForbidden) {
		t.Fatalf("project viewer review should be forbidden: %v", err)
	}
	if _, _, err := service.Review(
		context.Background(),
		reviewer,
		firstRelease.ReleaseID,
		StatusApproved,
		"",
	); err != nil {
		t.Fatal(err)
	}
	if _, _, err := service.Publish(
		context.Background(),
		editor,
		firstRelease.ReleaseID,
	); !errors.Is(err, ErrForbidden) {
		t.Fatalf("editor publish should be forbidden: %v", err)
	}
	if _, _, err := service.Publish(
		context.Background(),
		owner,
		firstRelease.ReleaseID,
	); err != nil {
		t.Fatal(err)
	}
	if _, err := service.Get(context.Background(), viewer, firstRelease.ReleaseID); err != nil {
		t.Fatalf("viewer could not read project release: %v", err)
	}
	if _, err := service.Get(context.Background(), outsider, firstRelease.ReleaseID); !errors.Is(err, ErrNotFound) {
		t.Fatalf("outsider release lookup was not hidden: %v", err)
	}
	if _, err := service.Download(context.Background(), viewer, firstRelease.ReleaseID); err != nil {
		t.Fatalf("private project player member could not download: %v", err)
	}
	if _, err := service.Download(context.Background(), outsider, firstRelease.ReleaseID); !errors.Is(err, ErrNotFound) {
		t.Fatalf("private project outsider download was not hidden: %v", err)
	}
	if work, err := service.GetWorkPage(context.Background(), viewer, project.ProjectID); err != nil || work.LatestRelease.ReleaseID != firstRelease.ReleaseID {
		t.Fatalf("private member work page mismatch: work=%+v err=%v", work, err)
	}
	if _, err := service.GetWorkPage(context.Background(), outsider, project.ProjectID); !errors.Is(err, ErrNotFound) {
		t.Fatalf("private outsider work page was not hidden: %v", err)
	}

	secondRelease, created, err := service.Submit(
		context.Background(),
		editor,
		"editor-submit-second-v1",
		second,
	)
	if err != nil || !created {
		t.Fatalf("second editor submit failed: created=%v release=%+v err=%v", created, secondRelease, err)
	}
	page, err := service.ListProjectVersions(
		context.Background(),
		viewer,
		project.ProjectID,
		"",
		1,
	)
	if err != nil || len(page.Releases) != 1 || page.NextCursor == "" {
		t.Fatalf("first version page mismatch: page=%+v err=%v", page, err)
	}
	nextPage, err := service.ListProjectVersions(
		context.Background(),
		viewer,
		project.ProjectID,
		page.NextCursor,
		1,
	)
	if err != nil || len(nextPage.Releases) != 1 || nextPage.NextCursor != "" ||
		nextPage.Releases[0].ReleaseID == page.Releases[0].ReleaseID {
		t.Fatalf("second version page mismatch: page=%+v err=%v", nextPage, err)
	}

	if _, changed, err := service.RemoveProjectMember(
		context.Background(),
		owner,
		project.ProjectID,
		editor.Subject,
	); err != nil || !changed {
		t.Fatalf("remove editor failed: changed=%v err=%v", changed, err)
	}
	third, _, _ := signedArtifact(t, "f1111111-2222-3333-4444-555555555555")
	if _, _, err := service.Submit(
		context.Background(),
		editor,
		"removed-editor-submit-v1",
		third,
	); !errors.Is(err, ErrForbidden) {
		t.Fatalf("removed editor submit should be forbidden: %v", err)
	}
	if _, _, err := service.RemoveProjectMember(
		context.Background(),
		owner,
		project.ProjectID,
		owner.Subject,
	); !errors.Is(err, ErrConflict) {
		t.Fatalf("project owner removal should conflict: %v", err)
	}
}

func createServiceTestProject(t *testing.T, service *Service, owner Principal) Project {
	t.Helper()
	project, created, err := service.CreateProject(
		context.Background(),
		owner,
		"create-project-fixture-v1",
		"aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee",
		"Registry test project",
	)
	if err != nil || !created {
		t.Fatalf("create fixture project failed: created=%v project=%+v err=%v", created, project, err)
	}
	return project
}
