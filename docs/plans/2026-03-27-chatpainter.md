# ChatPainter Implementation Plan

See CONTINUE.md for context. This is the Telegram-style QPainter approach.

## Phase 1: Skeleton (compiles, blank item)
- Create src/painter/ directory with ChatPainter, MessageLayout, LayoutEngine, ImageCache, PainterTheme
- Register in CMakeLists.txt + main.cpp
- Add ChatPainter {} to ChatView.qml alongside ListView

## Phase 2: Layout engine (correct heights)
- LayoutEngine::computeLayout() with QTextDocument for body text
- Connect to model signals, rebuild layouts on change

## Phase 3: Basic painting (messages visible)
- Date separators, system messages, own bubbles, other messages
- Avatar circles with initials, name labels, timestamps

## Phase 4: Scrolling
- Mouse wheel + drag scroll
- History load on scroll-to-top
- Auto-scroll on new messages
- Scroll-to-bottom button

## Phase 5: Images
- ImageCache bridges AvatarProvider + FilePreviewProvider
- Async load → repaint on ready

## Phase 6: Reply quotes + reactions

## Phase 7: Interactions (click, hover, right-click)

## Phase 8: Hover action bar (react/reply buttons)

## Phase 9: Theme sync (dark mode, font scale)

## Phase 10: Switch off ListView
