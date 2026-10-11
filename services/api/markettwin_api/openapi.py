"""``python -m markettwin_api.openapi``: print the API's OpenAPI document.

The terminal's TypeScript types are generated from this output, which is committed as
``apps/web/openapi.json``.
"""

from __future__ import annotations

import json

from markettwin_api.app import create_app


def main() -> None:
    print(json.dumps(create_app().openapi(), indent=2))


if __name__ == "__main__":
    main()
