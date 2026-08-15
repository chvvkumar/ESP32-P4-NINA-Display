# OctoPrint Application Keys protocol

Source: https://raw.githubusercontent.com/OctoPrint/OctoPrint/dev/docs/bundledplugins/appkeys.rst
Extracted 2026-08-14. Verified against live OctoPrint 1.11.8 at octopi.lan for the probe endpoint only
(read-only capture; no POST was issued against the live instance).

## Path note

The plugin path segment is `appkeys` (singular `key` is wrong, plural `keys` is correct):

```
/plugin/appkeys/probe
/plugin/appkeys/request
/plugin/appkeys/request/<app_token>
/plugin/appkeys/decision/<user_token>
/api/plugin/appkeys
```

There is no `/plugin/appkey/` and no `/api/appkeys`. The workflow endpoints live under `/plugin/appkeys/`,
while key management lives under `/api/plugin/appkeys`.

## Endpoints the mock must implement

### 1. GET /plugin/appkeys/probe

Probes for workflow support. No authentication.

- 204: workflow is supported. Empty body.
- Any other status (typically 404): plugin disabled or not installed, client must fall back to
  manual copy-paste of an API key.

Observed live response headers on OctoPrint 1.11.8: `content-type: text/plain`, `content-length: 0`,
`vary: Cookie`.

### 2. POST /plugin/appkeys/request

Starts the authorization process. No authentication (this is the pre-auth handshake).

Request body (JSON):

| Field  | Required | Type | Description |
|--------|----------|------|-------------|
| `app`  | yes      | str  | Human readable application identifier shown to the user. Treated case insensitively, so `My App` and `my APP` are the same application. |
| `user` | no       | str  | User id to restrict the decision to. If omitted, any logged-in user may grant the request. |

Response: 201 Created.

- `Location` header carries the request-specific polling endpoint:
  `/plugin/appkeys/request/<app_token>`.
- Body is the Authorization response:

| Field         | Type | Description |
|---------------|------|-------------|
| `app_token`   | str  | Token used to poll for the decision. |
| `auth_dialog` | str  | URL of a lightweight login and confirmation dialog the app can open in a browser window. Added in OctoPrint 1.8.0. |

The server simultaneously pushes a plugin message named `appkeys` over the OctoPrint websocket carrying
the app name, the `user_token`, and the user id, which is what makes the confirmation prompt appear in the
web interface.

### 3. GET /plugin/appkeys/request/&lt;app_token&gt;

Polls for the decision. No authentication. The client polls every 1 second.

- 202: no decision yet, keep polling.
- 200: access granted. Body is the Key response: `{"api_key": "<str>"}`.
- 404: access denied, or the request timed out or went stale.

Expiry behavior: the server considers a pending request stale and deletes it internally if this polling
endpoint is not called for more than 5 seconds. A mock must therefore either replicate that 5 second
idle timeout or document that it does not, because a client that stops polling and resumes later is
expected to receive 404, not 202.

### 4. POST /plugin/appkeys/decision/&lt;user_token&gt;

Records the user's decision. Called by the web interface or the auth dialog, not by the app.

Requires the `PLUGIN_APPKEYS_GRANT` permission and a recent credentials check.

Request body (JSON):

| Field      | Required | Type | Description |
|------------|----------|------|-------------|
| `decision` | yes      | bool | `true` grants access, `false` denies it. |

Response: 204 No Content on success.

### 5. GET /api/plugin/appkeys

Lists existing keys and pending requests for the current user. Requires an API key.

Query parameters:

- `all`: list keys and pending requests for all users. Requires `PLUGIN_APPKEYS_ADMIN`.
- `app`: restrict to the key for one application identifier.
- `user`: with `app`, fetch another user's key. Requires `PLUGIN_APPKEYS_ADMIN`.

Response 200, List response:

| Field     | Type | Description |
|-----------|------|-------------|
| `keys`    | list | Key list entries: `api_key`, `app_id`, `user_id`. |
| `pending` | list | Pending list entries: `app_id`, optional `user_id`, `user_token`. |

### 6. POST /api/plugin/appkeys

Key management commands. Requires an API key, user rights, and a fresh credentials check.

| Field     | Type | Description |
|-----------|------|-------------|
| `command` | str  | `revoke` or `generate`. |
| `app`     | str  | Application identifier to revoke or generate for. |
| `user`    | str  | Optional user id, defaults to the current user. Admin only for other users. |
| `key`     | str  | `revoke` only. Deprecated since 1.10.0, use `app` plus optional `user`. |

Status codes: 200 on successful `generate` (body `{"app_id", "user_id", "api_key"}`), 204 on successful
`revoke`, 400 on an invalid or missing parameter.

## Status code summary

| Code | Where | Meaning |
|------|-------|---------|
| 204  | probe | Workflow supported. |
| 404  | probe | Plugin absent, fall back to manual key entry. |
| 201  | POST request | Authorization process started, `Location` header holds the polling URL. |
| 202  | GET request/&lt;app_token&gt; | Decision pending, poll again in 1 second. |
| 200  | GET request/&lt;app_token&gt; | Granted, body carries `api_key`. |
| 404  | GET request/&lt;app_token&gt; | Denied, timed out, or stale. |
| 204  | POST decision/&lt;user_token&gt; | Decision recorded. |
| 200 / 204 / 400 | /api/plugin/appkeys | generate / revoke / bad parameter. |

## Client flow the mock must satisfy

1. GET `/plugin/appkeys/probe`. Not 204 means abandon the workflow.
2. POST `/plugin/appkeys/request` with `app` and optionally `user`.
3. Read `app_token` from the body, or the polling URL from the `Location` header. Read `auth_dialog`
   if the client wants to open the lightweight confirmation dialog rather than the full web interface.
4. Poll GET `/plugin/appkeys/request/<app_token>` every 1 second, without gaps longer than 5 seconds.
5. On 200, store `api_key` and send it as `X-Api-Key` on all later requests. On 404, report denial.

## Discrepancy in the upstream document

The sequence diagrams in the document label the request body fields `app_name` and `user_id`. The API
reference section and the data model table both name them `app` and `user`. The API reference and data
model are authoritative; a mock should accept `app` and `user`. Accepting `app_name` as an alias costs
nothing and guards against clients written from the diagram.
