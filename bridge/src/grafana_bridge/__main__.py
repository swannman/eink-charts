from __future__ import annotations

import logging
import os

import uvicorn


def main() -> None:
    logging.basicConfig(
        level=os.environ.get("LOG_LEVEL", "INFO").upper(),
        format="%(asctime)s %(levelname)s %(name)s %(message)s",
    )
    host = os.environ.get("LISTEN_HOST", "0.0.0.0")
    port = int(os.environ.get("LISTEN_PORT", "8080"))
    uvicorn.run("grafana_bridge.app:app", host=host, port=port, log_level="info")


if __name__ == "__main__":
    main()
