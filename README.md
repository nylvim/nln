# nln (Net LiNk)

A tiny short link server in C.

## Usage

- If running for the first time, create an empty database file.
- Set the `NLN_AUTH_TOKEN` environment variable.
- Run `nln [PORT]`. Default port is 6174.
- Send `SIGINT` or `SIGTERM` to stop the server.

The `nlc` script contains a simple client using cURL.

## API

**Redirect:** `GET /<link>` -> 301/302 or 404

**Read link:** `GET /<link>?read=true` -> 200 (returns target) or 404

**Create link:** `POST /` (with url) -> 200 (returns relative path of the link)

**Set link:** `PUT /<link>` (with url) -> 204 or 409 (returns existing target)

**Force set link:** `PUT /<link>?force=true` (with url) -> 200 (returns existing target) or 204

**Delete link:** `DELETE /<link>` -> 200 (returns target) or 204

**Delete all links to URL:** `POST /?delete=true` (with url) -> 200 (returns number of links deleted)

---

`HEAD` requests are also accepted at `GET` endpoints.

`POST` / `PUT` / `DELETE` requests must include `Authorization: Bearer <token>` in the header.

The request body should be a single line of plain text if present, so is the response body.

Other possible status codes:

- 400, 403, 405. These should be self-explanatory.
- 406 for `PUT` requests, if requested path is too short or too long.
- 413, 422 for `POST` / `PUT` requests, if provided URL is too long or illegal.
- 500 for `POST` requests that create links, if no spare link can be found.
