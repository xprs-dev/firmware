# Working in firmware

## The web chat page is one page

The page is developed only in `portal` (`page/chat_page.html`, built by
`tool/build.py`) and every station serves the identical build: the firmware
(`common/xprs_hotspot/chat_page.c`, `build.py --c`), the server
(`web/chat_page.html` and `lib/web/embedded_page.dart`, `tool/sync_web_chat.sh`)
and the app (`assets/web_chat/chat_page.html`). Never edit a copy. After a
portal change, regenerate every copy in the same change and check they match.

The only part that differs per implementation is the API engine behind the
page (`/api/...`). Keep it generic and XPRS-compliant: what a station can or
cannot do reaches the page as data (`/api/services`: `page.tabs`, features),
never as a different page or a feature placed somewhere else on one kind of
station. Feature logic belongs in the page's JavaScript.
