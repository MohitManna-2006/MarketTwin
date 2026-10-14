"""HTTP and WebSocket surface of the live market-data hub.

Reading is open to whoever can reach the API, like the rest of this local service.
Changing the upstream connection is not: starting or stopping the feed, editing the
subscription, and recording act on the single provider session and on local storage, so
they are accepted only from this machine or with the configured control token. Browser
requests from another site's origin are refused on both surfaces, because a WebSocket is
not protected by CORS.
"""

from __future__ import annotations

import hmac
from ipaddress import ip_address
from typing import Annotated, Any, Literal
from urllib.parse import urlsplit

from fastapi import APIRouter, Depends, Query, Request, WebSocket, WebSocketDisconnect
from pydantic import BaseModel, Field

from markettwin_api.live_service import LiveService
from markettwin_api.service import ServiceError
from markettwin_api.settings import Settings

router = APIRouter(prefix="/api/v1/live", tags=["Live market data"])

_LOCAL_NAMES = ("localhost", "testclient")


def _loopback(host: str | None) -> bool:
    if not host:
        return False
    if host in _LOCAL_NAMES:
        return True
    try:
        return ip_address(host.strip("[]")).is_loopback
    except ValueError:
        return False


def origin_allowed(origin: str | None, settings: Settings) -> bool:
    """A request with no Origin is not from a browser page. One with an Origin must come
    from the terminal: a configured origin, or any page served from this machine."""
    if origin is None:
        return True
    return origin in settings.cors_origins or _loopback(urlsplit(origin).hostname)


def get_live(request: Request) -> LiveService:
    live: LiveService = request.app.state.live
    return live


def require_control(request: Request) -> LiveService:
    live: LiveService = request.app.state.live
    settings = live.settings
    if not origin_allowed(request.headers.get("origin"), settings):
        raise ServiceError(403, "OriginNotAllowed", "live controls are not open to this origin")
    token = request.headers.get("x-markettwin-control-token", "")
    if settings.live_control_token and hmac.compare_digest(token, settings.live_control_token):
        return live
    if _loopback(request.client.host if request.client else None):
        return live
    raise ServiceError(
        403,
        "ControlNotPermitted",
        "live controls are accepted from this machine, or with the control token",
    )


Live = Annotated[LiveService, Depends(get_live)]
Control = Annotated[LiveService, Depends(require_control)]


class StartFeedRequest(BaseModel):
    feed: Literal["test", "iex", "mock"] | None = None


class SymbolsRequest(BaseModel):
    symbols: list[Annotated[str, Field(max_length=12)]] = Field(max_length=256)
    feed: Literal["test", "iex", "mock"] | None = None


@router.get("/status")
async def status(live: Live) -> Any:
    """Feed connection, market activity, pipeline, readers, and recording."""
    return live.status()


@router.get("/health")
async def health(live: Live) -> Any:
    return live.health()


@router.get("/symbols")
async def symbols(live: Live) -> Any:
    return live.symbols()


@router.get("/snapshot")
async def snapshot(live: Live) -> Any:
    """The same document a stream reader receives first: status, states, recent events."""
    return live.snapshot()


@router.post("/symbols")
async def set_symbols(body: SymbolsRequest, live: Control) -> Any:
    return await live.set_symbols(body.symbols, body.feed)


@router.post("/feed/start")
async def start_feed(body: StartFeedRequest, live: Control) -> Any:
    return await live.start_feed(body.feed)


@router.post("/feed/stop")
async def stop_feed(live: Control) -> Any:
    return await live.stop_feed()


@router.get("/recordings")
async def recordings(live: Live, limit: Annotated[int, Query(ge=1, le=100)] = 20) -> Any:
    return live.recordings(limit)


@router.post("/recordings", status_code=201)
async def start_recording(live: Control) -> Any:
    return await live.start_recording()


@router.post("/recordings/stop", status_code=202)
async def stop_recording(live: Control) -> Any:
    return await live.stop_recording()


@router.get("/recordings/{recording_id}")
async def recording(recording_id: str, live: Live) -> Any:
    return live.recording(recording_id)


@router.websocket("/stream")
async def stream(websocket: WebSocket) -> None:
    live: LiveService = websocket.app.state.live
    if not origin_allowed(websocket.headers.get("origin"), live.settings):
        await websocket.close(code=1008, reason="origin not allowed")
        return
    await websocket.accept()
    try:
        await live.serve(websocket)
    except WebSocketDisconnect:
        return
