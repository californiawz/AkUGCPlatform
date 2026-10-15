package registry

import (
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"strconv"
	"strings"
	"unicode/utf8"
)

const maxControlBodyBytes = 4096

type HTTPHandler struct {
	service *Service
	auth    Authenticator
	mux     *http.ServeMux
}

func NewHTTPHandler(service *Service, auth Authenticator) http.Handler {
	handler := &HTTPHandler{
		service: service,
		auth:    auth,
		mux:     http.NewServeMux(),
	}
	handler.mux.HandleFunc("GET /healthz", handler.health)
	handler.mux.HandleFunc("POST /v1/projects", handler.createProject)
	handler.mux.HandleFunc("GET /v1/projects/{projectID}", handler.getProject)
	handler.mux.HandleFunc("GET /v1/projects/{projectID}/members", handler.listProjectMembers)
	handler.mux.HandleFunc("PUT /v1/projects/{projectID}/members/{subject}", handler.setProjectMember)
	handler.mux.HandleFunc("DELETE /v1/projects/{projectID}/members/{subject}", handler.removeProjectMember)
	handler.mux.HandleFunc("GET /v1/projects/{projectID}/versions", handler.listProjectVersions)
	handler.mux.HandleFunc("POST /v1/releases", handler.submit)
	handler.mux.HandleFunc("GET /v1/releases/{releaseID}", handler.get)
	handler.mux.HandleFunc("POST /v1/releases/{releaseID}/review", handler.review)
	handler.mux.HandleFunc("POST /v1/releases/{releaseID}/publish", handler.publish)
	handler.mux.HandleFunc("POST /v1/releases/{releaseID}/revoke", handler.revoke)
	handler.mux.HandleFunc("GET /v1/releases/{releaseID}/download", handler.download)
	return handler
}

func (h *HTTPHandler) ServeHTTP(response http.ResponseWriter, request *http.Request) {
	response.Header().Set("X-Content-Type-Options", "nosniff")
	response.Header().Set("Cache-Control", "no-store")
	h.mux.ServeHTTP(response, request)
}

func (h *HTTPHandler) health(response http.ResponseWriter, _ *http.Request) {
	writeJSON(response, http.StatusOK, map[string]any{
		"status": "ok",
	})
}

type createProjectRequest struct {
	ProjectID   string `json:"projectId"`
	DisplayName string `json:"displayName"`
}

func (h *HTTPHandler) createProject(response http.ResponseWriter, request *http.Request) {
	principal, ok := h.authenticate(response, request)
	if !ok {
		return
	}
	if err := requireJSONContentType(request); err != nil {
		writeAPIError(response, err)
		return
	}
	var body createProjectRequest
	if err := decodeControlBody(response, request, &body, "projectId", "displayName"); err != nil {
		writeAPIError(response, err)
		return
	}
	project, created, err := h.service.CreateProject(
		request.Context(),
		principal,
		request.Header.Get("Idempotency-Key"),
		body.ProjectID,
		body.DisplayName,
	)
	if err != nil {
		writeAPIError(response, err)
		return
	}
	status := http.StatusOK
	if created {
		status = http.StatusCreated
	}
	response.Header().Set("Location", "/v1/projects/"+project.ProjectID)
	writeJSON(response, status, map[string]any{
		"created": created,
		"project": project,
	})
}

func (h *HTTPHandler) getProject(response http.ResponseWriter, request *http.Request) {
	principal, ok := h.authenticate(response, request)
	if !ok {
		return
	}
	if err := requireNoQuery(request); err != nil {
		writeAPIError(response, err)
		return
	}
	project, role, err := h.service.GetProject(
		request.Context(),
		principal,
		request.PathValue("projectID"),
	)
	if err != nil {
		writeAPIError(response, err)
		return
	}
	writeJSON(response, http.StatusOK, map[string]any{
		"accessRole": role,
		"project":    project,
	})
}

func (h *HTTPHandler) listProjectMembers(response http.ResponseWriter, request *http.Request) {
	principal, ok := h.authenticate(response, request)
	if !ok {
		return
	}
	if err := requireNoQuery(request); err != nil {
		writeAPIError(response, err)
		return
	}
	members, err := h.service.ListProjectMembers(
		request.Context(),
		principal,
		request.PathValue("projectID"),
	)
	if err != nil {
		writeAPIError(response, err)
		return
	}
	writeJSON(response, http.StatusOK, map[string]any{"members": members})
}

type setProjectMemberRequest struct {
	Role ProjectRole `json:"role"`
}

func (h *HTTPHandler) setProjectMember(response http.ResponseWriter, request *http.Request) {
	principal, ok := h.authenticate(response, request)
	if !ok {
		return
	}
	if err := requireNoQuery(request); err != nil {
		writeAPIError(response, err)
		return
	}
	if err := requireJSONContentType(request); err != nil {
		writeAPIError(response, err)
		return
	}
	var body setProjectMemberRequest
	if err := decodeControlBody(response, request, &body, "role"); err != nil {
		writeAPIError(response, err)
		return
	}
	member, project, changed, err := h.service.SetProjectMember(
		request.Context(),
		principal,
		request.PathValue("projectID"),
		request.PathValue("subject"),
		body.Role,
	)
	if err != nil {
		writeAPIError(response, err)
		return
	}
	writeJSON(response, http.StatusOK, map[string]any{
		"changed": changed,
		"member":  member,
		"project": project,
	})
}

func (h *HTTPHandler) removeProjectMember(response http.ResponseWriter, request *http.Request) {
	principal, ok := h.authenticate(response, request)
	if !ok {
		return
	}
	if err := requireNoQuery(request); err != nil {
		writeAPIError(response, err)
		return
	}
	if err := requireEmptyBody(response, request); err != nil {
		writeAPIError(response, err)
		return
	}
	project, changed, err := h.service.RemoveProjectMember(
		request.Context(),
		principal,
		request.PathValue("projectID"),
		request.PathValue("subject"),
	)
	if err != nil {
		writeAPIError(response, err)
		return
	}
	writeJSON(response, http.StatusOK, map[string]any{
		"changed": changed,
		"project": project,
	})
}

func (h *HTTPHandler) listProjectVersions(response http.ResponseWriter, request *http.Request) {
	principal, ok := h.authenticate(response, request)
	if !ok {
		return
	}
	values, err := parseVersionQuery(request)
	if err != nil {
		writeAPIError(response, err)
		return
	}
	limit := 20
	if rawLimit := values.Get("limit"); rawLimit != "" {
		limit, err = strconv.Atoi(rawLimit)
		if err != nil {
			writeAPIError(response, fmt.Errorf("%w: version limit must be an integer", ErrInvalidInput))
			return
		}
	}
	page, err := h.service.ListProjectVersions(
		request.Context(),
		principal,
		request.PathValue("projectID"),
		values.Get("cursor"),
		limit,
	)
	if err != nil {
		writeAPIError(response, err)
		return
	}
	writeJSON(response, http.StatusOK, page)
}

func (h *HTTPHandler) submit(response http.ResponseWriter, request *http.Request) {
	principal, ok := h.authenticate(response, request)
	if !ok {
		return
	}
	if contentType := request.Header.Get("Content-Type"); contentType != "application/vnd.akugc+json" {
		writeAPIError(response, fmt.Errorf("%w: Content-Type must be application/vnd.akugc+json", ErrInvalidInput))
		return
	}
	artifact, err := readBody(response, request, MaxArtifactBytes)
	if err != nil {
		writeAPIError(response, err)
		return
	}
	release, created, err := h.service.Submit(
		request.Context(),
		principal,
		request.Header.Get("Idempotency-Key"),
		artifact,
	)
	if err != nil {
		writeAPIError(response, err)
		return
	}
	status := http.StatusOK
	if created {
		status = http.StatusCreated
	}
	response.Header().Set("Location", "/v1/releases/"+release.ReleaseID)
	writeJSON(response, status, map[string]any{
		"created": created,
		"release": release,
	})
}

func (h *HTTPHandler) get(response http.ResponseWriter, request *http.Request) {
	principal, ok := h.authenticate(response, request)
	if !ok {
		return
	}
	release, err := h.service.Get(request.Context(), principal, request.PathValue("releaseID"))
	if err != nil {
		writeAPIError(response, err)
		return
	}
	writeJSON(response, http.StatusOK, map[string]any{"release": release})
}

type reviewRequest struct {
	Decision Status `json:"decision"`
	Reason   string `json:"reason"`
}

func (h *HTTPHandler) review(response http.ResponseWriter, request *http.Request) {
	principal, ok := h.authenticate(response, request)
	if !ok {
		return
	}
	var body reviewRequest
	if err := decodeControlBody(response, request, &body, "decision", "reason"); err != nil {
		writeAPIError(response, err)
		return
	}
	release, changed, err := h.service.Review(
		request.Context(),
		principal,
		request.PathValue("releaseID"),
		body.Decision,
		body.Reason,
	)
	if err != nil {
		writeAPIError(response, err)
		return
	}
	writeJSON(response, http.StatusOK, map[string]any{
		"changed": changed,
		"release": release,
	})
}

func (h *HTTPHandler) publish(response http.ResponseWriter, request *http.Request) {
	principal, ok := h.authenticate(response, request)
	if !ok {
		return
	}
	if err := requireEmptyBody(response, request); err != nil {
		writeAPIError(response, err)
		return
	}
	release, changed, err := h.service.Publish(
		request.Context(),
		principal,
		request.PathValue("releaseID"),
	)
	if err != nil {
		writeAPIError(response, err)
		return
	}
	writeJSON(response, http.StatusOK, map[string]any{
		"changed": changed,
		"release": release,
	})
}

type revokeRequest struct {
	Reason string `json:"reason"`
}

func (h *HTTPHandler) revoke(response http.ResponseWriter, request *http.Request) {
	principal, ok := h.authenticate(response, request)
	if !ok {
		return
	}
	var body revokeRequest
	if err := decodeControlBody(response, request, &body, "reason"); err != nil {
		writeAPIError(response, err)
		return
	}
	release, changed, err := h.service.Revoke(
		request.Context(),
		principal,
		request.PathValue("releaseID"),
		body.Reason,
	)
	if err != nil {
		writeAPIError(response, err)
		return
	}
	writeJSON(response, http.StatusOK, map[string]any{
		"changed": changed,
		"release": release,
	})
}

func (h *HTTPHandler) download(response http.ResponseWriter, request *http.Request) {
	principal, ok := h.authenticate(response, request)
	if !ok {
		return
	}
	release, err := h.service.Download(request.Context(), principal, request.PathValue("releaseID"))
	if err != nil {
		writeAPIError(response, err)
		return
	}
	response.Header().Set("Content-Type", "application/vnd.akugc+json")
	response.Header().Set("Content-Length", strconv.FormatInt(release.ArtifactSize, 10))
	response.Header().Set("Content-Disposition", `attachment; filename="`+release.ReleaseID+`.akugc"`)
	response.Header().Set("ETag", `"sha256:`+release.ArtifactSHA256+`"`)
	response.Header().Set("X-AkUGC-Content-Hash", release.ContentHash)
	response.WriteHeader(http.StatusOK)
	_, _ = response.Write(release.Artifact)
}

func (h *HTTPHandler) authenticate(
	response http.ResponseWriter,
	request *http.Request,
) (Principal, bool) {
	if h.auth == nil {
		writeAPIError(response, ErrUnavailable)
		return Principal{}, false
	}
	principal, err := h.auth.Authenticate(request.Header.Get("Authorization"))
	if err != nil {
		writeAPIError(response, ErrUnauthenticated)
		return Principal{}, false
	}
	return principal, true
}

func decodeControlBody(
	response http.ResponseWriter,
	request *http.Request,
	destination any,
	requiredFields ...string,
) error {
	raw, err := readBody(response, request, maxControlBodyBytes)
	if err != nil {
		return err
	}
	if !utf8.Valid(raw) {
		return fmt.Errorf("%w: request body is not valid UTF-8", ErrInvalidInput)
	}
	if err := validateJSONShape(raw); err != nil {
		return fmt.Errorf("%w: %v", ErrInvalidInput, err)
	}
	if _, err := requireExactObjectFields(raw, "request body", requiredFields); err != nil {
		return fmt.Errorf("%w: %v", ErrInvalidInput, err)
	}
	decoder := json.NewDecoder(bytes.NewReader(raw))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(destination); err != nil {
		return fmt.Errorf("%w: invalid JSON body", ErrInvalidInput)
	}
	if err := requireJSONEOF(decoder); err != nil {
		return fmt.Errorf("%w: trailing JSON body", ErrInvalidInput)
	}
	return nil
}

func requireJSONContentType(request *http.Request) error {
	if request.Header.Get("Content-Type") != "application/json" {
		return fmt.Errorf("%w: Content-Type must be application/json", ErrInvalidInput)
	}
	return nil
}

func requireNoQuery(request *http.Request) error {
	if request.URL.RawQuery != "" {
		return fmt.Errorf("%w: query parameters are not supported", ErrInvalidInput)
	}
	return nil
}

func parseVersionQuery(request *http.Request) (url.Values, error) {
	values, err := url.ParseQuery(request.URL.RawQuery)
	if err != nil {
		return nil, fmt.Errorf("%w: invalid query parameters", ErrInvalidInput)
	}
	for name, entries := range values {
		if name != "cursor" && name != "limit" {
			return nil, fmt.Errorf("%w: unknown query parameter %q", ErrInvalidInput, name)
		}
		if len(entries) != 1 {
			return nil, fmt.Errorf("%w: query parameter %q must occur once", ErrInvalidInput, name)
		}
		if entries[0] == "" {
			return nil, fmt.Errorf("%w: query parameter %q cannot be empty", ErrInvalidInput, name)
		}
	}
	return values, nil
}

func requireEmptyBody(response http.ResponseWriter, request *http.Request) error {
	raw, err := readBody(response, request, 1)
	if err != nil {
		return err
	}
	if len(strings.TrimSpace(string(raw))) != 0 {
		return fmt.Errorf("%w: request body must be empty", ErrInvalidInput)
	}
	return nil
}

func readBody(response http.ResponseWriter, request *http.Request, limit int64) ([]byte, error) {
	request.Body = http.MaxBytesReader(response, request.Body, limit)
	raw, err := io.ReadAll(request.Body)
	if err != nil {
		var tooLarge *http.MaxBytesError
		if errors.As(err, &tooLarge) {
			return nil, fmt.Errorf("%w: request body exceeds %d bytes", ErrInvalidInput, limit)
		}
		return nil, fmt.Errorf("%w: read request body", ErrInvalidInput)
	}
	return raw, nil
}

func writeJSON(response http.ResponseWriter, status int, value any) {
	response.Header().Set("Content-Type", "application/json; charset=utf-8")
	response.WriteHeader(status)
	_ = json.NewEncoder(response).Encode(value)
}

func writeAPIError(response http.ResponseWriter, err error) {
	status := http.StatusInternalServerError
	code := "internal"
	message := "internal server error"
	switch {
	case errors.Is(err, ErrUnauthenticated):
		status, code, message = http.StatusUnauthorized, "unauthenticated", "authentication is required"
		response.Header().Set("WWW-Authenticate", "Bearer")
	case errors.Is(err, ErrForbidden):
		status, code, message = http.StatusForbidden, "forbidden", err.Error()
	case errors.Is(err, ErrInvalidInput):
		status, code, message = http.StatusBadRequest, "invalid_input", err.Error()
	case errors.Is(err, ErrNotFound):
		status, code, message = http.StatusNotFound, "not_found", "resource was not found"
	case errors.Is(err, ErrConflict):
		status, code, message = http.StatusConflict, "conflict", err.Error()
	case errors.Is(err, ErrUnavailable):
		status, code, message = http.StatusServiceUnavailable, "unavailable", "service dependency is unavailable"
	}
	writeJSON(response, status, map[string]any{
		"error": map[string]string{
			"code":    code,
			"message": message,
		},
	})
}
