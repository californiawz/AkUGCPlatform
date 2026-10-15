package registry

import (
	"bytes"
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"testing"
)

func TestHTTPLifecycleAndPublishedOnlyDownload(t *testing.T) {
	artifact, _, publicKey := signedArtifact(t, "71111111-2222-3333-4444-555555555555")
	handler := newTestHTTPHandler(t, publicKey)
	createHTTPTestProject(t, handler)

	unauthenticated := performRequest(handler, http.MethodPost, "/v1/releases", artifact, "", map[string]string{
		"Content-Type":    "application/vnd.akugc+json",
		"Idempotency-Key": "http-submit-71111111",
	})
	if unauthenticated.Code != http.StatusUnauthorized {
		t.Fatalf("unauthenticated status = %d", unauthenticated.Code)
	}

	submitted := performRequest(handler, http.MethodPost, "/v1/releases", artifact, "creator-token", map[string]string{
		"Content-Type":    "application/vnd.akugc+json",
		"Idempotency-Key": "http-submit-71111111",
	})
	if submitted.Code != http.StatusCreated {
		t.Fatalf("submit status = %d body=%s", submitted.Code, submitted.Body.String())
	}
	retry := performRequest(handler, http.MethodPost, "/v1/releases", artifact, "creator-token", map[string]string{
		"Content-Type":    "application/vnd.akugc+json",
		"Idempotency-Key": "http-submit-71111111",
	})
	if retry.Code != http.StatusOK {
		t.Fatalf("retry status = %d body=%s", retry.Code, retry.Body.String())
	}

	releasePath := "/v1/releases/71111111-2222-3333-4444-555555555555"
	beforePublish := performRequest(handler, http.MethodGet, releasePath+"/download", nil, "player-token", nil)
	if beforePublish.Code != http.StatusNotFound {
		t.Fatalf("unpublished download status = %d", beforePublish.Code)
	}
	reviewed := performRequest(
		handler,
		http.MethodPost,
		releasePath+"/review",
		[]byte(`{"decision":"approved","reason":"ok"}`),
		"reviewer-token",
		nil,
	)
	if reviewed.Code != http.StatusOK {
		t.Fatalf("review status = %d body=%s", reviewed.Code, reviewed.Body.String())
	}
	forbiddenPublish := performRequest(
		handler,
		http.MethodPost,
		releasePath+"/publish",
		nil,
		"player-token",
		nil,
	)
	if forbiddenPublish.Code != http.StatusForbidden {
		t.Fatalf("player publish status = %d", forbiddenPublish.Code)
	}
	published := performRequest(
		handler,
		http.MethodPost,
		releasePath+"/publish",
		nil,
		"creator-token",
		nil,
	)
	if published.Code != http.StatusOK {
		t.Fatalf("publish status = %d body=%s", published.Code, published.Body.String())
	}
	privateDownload := performRequest(handler, http.MethodGet, releasePath+"/download", nil, "player-token", nil)
	if privateDownload.Code != http.StatusNotFound {
		t.Fatalf("private download status = %d body=%s", privateDownload.Code, privateDownload.Body.String())
	}
	workPath := "/v1/works/aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee"
	privateWork := performRequest(handler, http.MethodGet, workPath, nil, "player-token", nil)
	if privateWork.Code != http.StatusNotFound {
		t.Fatalf("private work status = %d body=%s", privateWork.Code, privateWork.Body.String())
	}
	outsiderVisibility := performRequest(
		handler,
		http.MethodPut,
		"/v1/projects/aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee/visibility",
		[]byte(`{"visibility":"public"}`),
		"player-token",
		map[string]string{"Content-Type": "application/json"},
	)
	if outsiderVisibility.Code != http.StatusNotFound {
		t.Fatalf("outsider visibility status = %d body=%s", outsiderVisibility.Code, outsiderVisibility.Body.String())
	}
	visibility := performRequest(
		handler,
		http.MethodPut,
		"/v1/projects/aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee/visibility",
		[]byte(`{"visibility":"public"}`),
		"creator-token",
		map[string]string{"Content-Type": "application/json"},
	)
	if visibility.Code != http.StatusOK {
		t.Fatalf("public visibility status = %d body=%s", visibility.Code, visibility.Body.String())
	}
	var visibilityResponse struct {
		Changed bool    `json:"changed"`
		Project Project `json:"project"`
	}
	decodeResponse(t, visibility, &visibilityResponse)
	if !visibilityResponse.Changed ||
		visibilityResponse.Project.Visibility != ProjectVisibilityPublic {
		t.Fatalf("public visibility response = %+v", visibilityResponse)
	}
	workResponse := performRequest(handler, http.MethodGet, workPath, nil, "player-token", nil)
	if workResponse.Code != http.StatusOK {
		t.Fatalf("public work status = %d body=%s", workResponse.Code, workResponse.Body.String())
	}
	var work struct {
		Work WorkPage `json:"work"`
	}
	decodeResponse(t, workResponse, &work)
	if work.Work.ProjectID != "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee" ||
		work.Work.LatestRelease.ReleaseID != "71111111-2222-3333-4444-555555555555" ||
		work.Work.LatestRelease.Artifact != nil {
		t.Fatalf("public work response = %+v", work.Work)
	}
	downloaded := performRequest(handler, http.MethodGet, releasePath+"/download", nil, "player-token", nil)
	if downloaded.Code != http.StatusOK || !bytes.Equal(downloaded.Body.Bytes(), artifact) {
		t.Fatalf("download failed: status=%d size=%d", downloaded.Code, downloaded.Body.Len())
	}
	if downloaded.Header().Get("ETag") == "" ||
		downloaded.Header().Get("Content-Type") != "application/vnd.akugc+json" {
		t.Fatalf("download headers are incomplete: %v", downloaded.Header())
	}
	revoked := performRequest(
		handler,
		http.MethodPost,
		releasePath+"/revoke",
		[]byte(`{"reason":"withdrawn"}`),
		"creator-token",
		nil,
	)
	if revoked.Code != http.StatusOK {
		t.Fatalf("revoke status = %d body=%s", revoked.Code, revoked.Body.String())
	}
	afterRevoke := performRequest(handler, http.MethodGet, releasePath+"/download", nil, "player-token", nil)
	if afterRevoke.Code != http.StatusNotFound {
		t.Fatalf("revoked download status = %d", afterRevoke.Code)
	}
	afterRevokeWork := performRequest(handler, http.MethodGet, workPath, nil, "player-token", nil)
	if afterRevokeWork.Code != http.StatusNotFound {
		t.Fatalf("revoked work status = %d", afterRevokeWork.Code)
	}
}

func TestHTTPRejectsDuplicateControlFields(t *testing.T) {
	artifact, _, publicKey := signedArtifact(t, "81111111-2222-3333-4444-555555555555")
	handler := newTestHTTPHandler(t, publicKey)
	createHTTPTestProject(t, handler)
	submitted := performRequest(handler, http.MethodPost, "/v1/releases", artifact, "creator-token", map[string]string{
		"Content-Type":    "application/vnd.akugc+json",
		"Idempotency-Key": "http-submit-81111111",
	})
	if submitted.Code != http.StatusCreated {
		t.Fatalf("submit status = %d", submitted.Code)
	}
	response := performRequest(
		handler,
		http.MethodPost,
		"/v1/releases/81111111-2222-3333-4444-555555555555/review",
		[]byte(`{"decision":"approved","decision":"rejected","reason":"ambiguous"}`),
		"reviewer-token",
		nil,
	)
	if response.Code != http.StatusBadRequest {
		t.Fatalf("duplicate decision status = %d body=%s", response.Code, response.Body.String())
	}
	response = performRequest(
		handler,
		http.MethodPost,
		"/v1/releases/81111111-2222-3333-4444-555555555555/review",
		[]byte(`{"Decision":"approved","reason":"ambiguous casing"}`),
		"reviewer-token",
		nil,
	)
	if response.Code != http.StatusBadRequest {
		t.Fatalf("case-variant decision status = %d body=%s", response.Code, response.Body.String())
	}
	response = performRequest(
		handler,
		http.MethodPut,
		"/v1/projects/aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee/visibility",
		[]byte(`{"visibility":"public","visibility":"private"}`),
		"creator-token",
		map[string]string{"Content-Type": "application/json"},
	)
	if response.Code != http.StatusBadRequest {
		t.Fatalf("duplicate visibility status = %d body=%s", response.Code, response.Body.String())
	}
}

func TestHTTPProjectCollaboratorsAndVersionListing(t *testing.T) {
	artifact, _, publicKey := signedArtifact(t, "82111111-2222-3333-4444-555555555555")
	handler := newTestHTTPHandler(t, publicKey)
	projectPath := "/v1/projects/aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee"
	createHTTPTestProject(t, handler)

	for subject, role := range map[string]string{
		"creator-editor": "editor",
		"project-viewer": "viewer",
	} {
		response := performRequest(
			handler,
			http.MethodPut,
			projectPath+"/members/"+subject,
			[]byte(`{"role":"`+role+`"}`),
			"creator-token",
			map[string]string{"Content-Type": "application/json"},
		)
		if response.Code != http.StatusOK {
			t.Fatalf("set %s status = %d body=%s", role, response.Code, response.Body.String())
		}
	}

	viewed := performRequest(handler, http.MethodGet, projectPath, nil, "viewer-token", nil)
	if viewed.Code != http.StatusOK {
		t.Fatalf("viewer project status = %d body=%s", viewed.Code, viewed.Body.String())
	}
	hidden := performRequest(handler, http.MethodGet, projectPath, nil, "outsider-token", nil)
	if hidden.Code != http.StatusNotFound {
		t.Fatalf("outsider project status = %d body=%s", hidden.Code, hidden.Body.String())
	}

	submitted := performRequest(
		handler,
		http.MethodPost,
		"/v1/releases",
		artifact,
		"editor-token",
		map[string]string{
			"Content-Type":    "application/vnd.akugc+json",
			"Idempotency-Key": "editor-http-submit-82111111",
		},
	)
	if submitted.Code != http.StatusCreated {
		t.Fatalf("editor submit status = %d body=%s", submitted.Code, submitted.Body.String())
	}
	var submission struct {
		Release Release `json:"release"`
	}
	decodeResponse(t, submitted, &submission)
	if submission.Release.OwnerSubject != "creator-a" ||
		submission.Release.SubmittedBySubject != "creator-editor" {
		t.Fatalf("submission attribution mismatch: %+v", submission.Release)
	}

	releasePath := "/v1/releases/82111111-2222-3333-4444-555555555555"
	memberReview := performRequest(
		handler,
		http.MethodPost,
		releasePath+"/review",
		[]byte(`{"decision":"approved","reason":""}`),
		"editor-token",
		map[string]string{"Content-Type": "application/json"},
	)
	if memberReview.Code != http.StatusForbidden {
		t.Fatalf("member review status = %d body=%s", memberReview.Code, memberReview.Body.String())
	}
	reviewed := performRequest(
		handler,
		http.MethodPost,
		releasePath+"/review",
		[]byte(`{"decision":"approved","reason":"independent"}`),
		"reviewer-token",
		map[string]string{"Content-Type": "application/json"},
	)
	if reviewed.Code != http.StatusOK {
		t.Fatalf("independent review status = %d body=%s", reviewed.Code, reviewed.Body.String())
	}
	editorPublish := performRequest(
		handler,
		http.MethodPost,
		releasePath+"/publish",
		nil,
		"editor-token",
		nil,
	)
	if editorPublish.Code != http.StatusForbidden {
		t.Fatalf("editor publish status = %d body=%s", editorPublish.Code, editorPublish.Body.String())
	}

	versions := performRequest(
		handler,
		http.MethodGet,
		projectPath+"/versions?limit=1",
		nil,
		"viewer-token",
		nil,
	)
	if versions.Code != http.StatusOK {
		t.Fatalf("version list status = %d body=%s", versions.Code, versions.Body.String())
	}
	var page ProjectVersionPage
	decodeResponse(t, versions, &page)
	if len(page.Releases) != 1 || page.Releases[0].Artifact != nil {
		t.Fatalf("version page mismatch: %+v", page)
	}
	badQuery := performRequest(
		handler,
		http.MethodGet,
		projectPath+"/versions?unknown=1",
		nil,
		"viewer-token",
		nil,
	)
	if badQuery.Code != http.StatusBadRequest {
		t.Fatalf("unknown query status = %d body=%s", badQuery.Code, badQuery.Body.String())
	}

	removed := performRequest(
		handler,
		http.MethodDelete,
		projectPath+"/members/creator-editor",
		nil,
		"creator-token",
		nil,
	)
	if removed.Code != http.StatusOK {
		t.Fatalf("remove editor status = %d body=%s", removed.Code, removed.Body.String())
	}
	removedSubmit := performRequest(
		handler,
		http.MethodPost,
		"/v1/releases",
		artifact,
		"editor-token",
		map[string]string{
			"Content-Type":    "application/vnd.akugc+json",
			"Idempotency-Key": "removed-http-submit-82111111",
		},
	)
	if removedSubmit.Code != http.StatusForbidden {
		t.Fatalf("removed editor submit status = %d body=%s", removedSubmit.Code, removedSubmit.Body.String())
	}
	removeOwner := performRequest(
		handler,
		http.MethodDelete,
		projectPath+"/members/creator-a",
		nil,
		"creator-token",
		nil,
	)
	if removeOwner.Code != http.StatusConflict {
		t.Fatalf("remove owner status = %d body=%s", removeOwner.Code, removeOwner.Body.String())
	}
}

func newTestHTTPHandler(t *testing.T, publicKey string) http.Handler {
	t.Helper()
	authenticator, err := NewStaticAuthenticator(AuthConfig{
		Principals: []AuthPrincipalConfig{
			{
				Subject:       "creator-a",
				TokenSHA256:   TokenSHA256("creator-token"),
				Roles:         []Role{RoleCreator, RolePublisher},
				PublisherKeys: []string{publicKey},
			},
			{
				Subject:     "reviewer",
				TokenSHA256: TokenSHA256("reviewer-token"),
				Roles:       []Role{RoleReviewer},
			},
			{
				Subject:     "player-b",
				TokenSHA256: TokenSHA256("player-token"),
				Roles:       []Role{RolePlayer},
			},
			{
				Subject:       "creator-editor",
				TokenSHA256:   TokenSHA256("editor-token"),
				Roles:         []Role{RoleCreator, RoleReviewer, RolePublisher},
				PublisherKeys: []string{publicKey},
			},
			{
				Subject:     "project-viewer",
				TokenSHA256: TokenSHA256("viewer-token"),
				Roles:       []Role{RolePlayer},
			},
			{
				Subject:     "project-outsider",
				TokenSHA256: TokenSHA256("outsider-token"),
				Roles:       []Role{RolePlayer},
			},
		},
	})
	if err != nil {
		t.Fatal(err)
	}
	return NewHTTPHandler(
		NewService(NewMemoryStore(), acceptingVerifier{}, nil),
		authenticator,
	)
}

func createHTTPTestProject(t *testing.T, handler http.Handler) {
	t.Helper()
	response := performRequest(
		handler,
		http.MethodPost,
		"/v1/projects",
		[]byte(`{"projectId":"aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee","displayName":"HTTP fixture"}`),
		"creator-token",
		map[string]string{
			"Content-Type":    "application/json",
			"Idempotency-Key": "create-http-project-v1",
		},
	)
	if response.Code != http.StatusCreated {
		t.Fatalf("create project status = %d body=%s", response.Code, response.Body.String())
	}
}

func performRequest(
	handler http.Handler,
	method string,
	path string,
	body []byte,
	token string,
	headers map[string]string,
) *httptest.ResponseRecorder {
	request := httptest.NewRequest(method, path, bytes.NewReader(body))
	if token != "" {
		request.Header.Set("Authorization", "Bearer "+token)
	}
	for name, value := range headers {
		request.Header.Set(name, value)
	}
	response := httptest.NewRecorder()
	handler.ServeHTTP(response, request)
	return response
}

func decodeResponse(t *testing.T, response *httptest.ResponseRecorder, destination any) {
	t.Helper()
	raw, err := io.ReadAll(response.Result().Body)
	if err != nil {
		t.Fatal(err)
	}
	if err := json.Unmarshal(raw, destination); err != nil {
		t.Fatalf("decode response: %v: %s", err, raw)
	}
}
