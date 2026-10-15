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
}

func TestHTTPRejectsDuplicateControlFields(t *testing.T) {
	artifact, _, publicKey := signedArtifact(t, "81111111-2222-3333-4444-555555555555")
	handler := newTestHTTPHandler(t, publicKey)
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
