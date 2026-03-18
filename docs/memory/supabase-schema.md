# Supabase Schema For Working + Mid-Term Memory

This document maps the current memory runtime model onto a Supabase/Postgres schema intended for:

- Full conversation history storage across a session
- Mid-term episode storage for always-present Tier 2 recall
- Future hydration of in-memory working state from SQL

It intentionally stops short of the long-term Knowledge Graph and long-term episodic vector store. Those are separate concerns in the memory plan.

## Scope

The current C++ runtime distinguishes two related but different persisted surfaces:

- `Conversation`: raw ordered chat history, stored as `ConversationItem`s containing either messages or an episode stub
- `Episode`: compacted mid-term memory rows created when an ongoing episode is flushed

That means the SQL layer should preserve both:

1. The exact chat log
2. The compacted mid-term representation produced from the chat log

## Recommended tables

### `memory_sessions`

One row per runtime session.

Suggested columns:

- `session_id text primary key`
- `user_id text not null`
- `system_prompt text not null`
- `created_at timestamptz not null`
- `ended_at timestamptz null`

### `conversation_items`

One row per ordered item in the session conversation timeline.

Suggested columns:

- `session_id text not null references memory_sessions(session_id) on delete cascade`
- `item_index bigint not null`
- `item_type text not null check (item_type in ('ongoing_episode', 'episode_stub'))`
- `episode_id text null`
- `episode_stub_content text null`
- `episode_stub_created_at timestamptz null`
- `check (...)` enforcing:
  - `ongoing_episode` rows must leave episode stub columns null
  - `episode_stub` rows must populate `episode_id`, `episode_stub_content`, and `episode_stub_created_at`
- `primary key (session_id, item_index)`

Notes:

- `item_index` preserves the exact timeline order used by the working-memory prompt.
- `episode_id` is nullable until an ongoing episode is flushed.
- The schema should enforce the same tagged-union contract as the C++ validator.
- `episode_id` should also reference `mid_term_episodes(episode_id)` once both tables exist.

### `conversation_messages`

One row per raw message inside an ongoing episode item.

Suggested columns:

- `session_id text not null`
- `item_index bigint not null`
- `message_index bigint not null`
- `turn_id text not null`
- `role text not null check (role in ('user', 'assistant'))`
- `content text not null`
- `created_at timestamptz not null`
- `primary key (session_id, item_index, message_index)`
- `foreign key (session_id, item_index) references conversation_items(session_id, item_index) on delete cascade`

Notes:

- `turn_id` comes from the gateway and is useful for traceability, replay, and dedupe.
- Keeping `message_index` explicit makes hydration deterministic.

### `mid_term_episodes`

One row per flushed episode.

Suggested columns:

- `episode_id text primary key`
- `session_id text not null references memory_sessions(session_id) on delete cascade`
- `source_item_index bigint not null`
- `tier1_detail text null`
- `tier2_summary text not null`
- `tier3_ref text not null`
- `tier3_keywords text[] not null default '{}'`
- `salience integer not null check (salience between 1 and 10)`
- `embedding jsonb not null default '[]'::jsonb`
- `created_at timestamptz not null`

Notes:

- `source_item_index` links the episode back to the original conversation position.
- `embedding` can start as `jsonb` if we want the simplest path first.
- If we later want similarity search inside Postgres, migrate `embedding` to `vector(n)` once the model dimension is fixed.

## Write flow mapping

The intended runtime write sequence is:

1. Upsert `memory_sessions` on the first turn of a session
2. Append raw rows into `conversation_items` and `conversation_messages` for each message
3. When a flush completes, upsert `mid_term_episodes`
4. Persist the conversation timeline update for the flushed item

For full flushes, the store can directly replace the matching `conversation_items` row with
`item_type = 'episode_stub'`, fill `episode_id`, `episode_stub_content`, and
`episode_stub_created_at`.

For split flushes, the store calls `split_conversation_item_with_episode_stub(...)` so the suffix
validation, item-index shifting, message move, completed-prefix delete, and episode-stub rewrite
happen transactionally inside Postgres.

This mirrors the current C++ architecture:

- raw messages stay preserved as the full transcript
- the conversation timeline still reflects stub replacement
- mid-term episodes remain queryable as an ordered list by `created_at`

## Hydration shape

To rebuild working memory for a session:

1. Load `memory_sessions`
2. Load `conversation_items` ordered by `item_index`
3. For `ongoing_episode` items, join `conversation_messages` ordered by `message_index`
4. Load `mid_term_episodes` ordered by `created_at`

That data maps directly to the new `MemoryStoreSnapshot` shape in C++.

## Supabase-specific notes

- Use the server-side service role on the Isla backend only; do not ship it to the desktop client.
- Keep Row Level Security decisions simple at first. If only the backend writes memory, service-role access can own the first version.
- Keep simple writes on direct PostgREST table upserts where possible.
- Use RPC-backed SQL functions for multi-row memory mutations that need fewer round trips or
  transactional guarantees. The split-flush path now uses
  `split_conversation_item_with_episode_stub(...)` for exactly that reason.

## Initial implementation recommendation

For the current serving path, implement:

- session upsert
- conversation message append
- episode upsert
- full-flush stub replacement
- split-flush RPC persistence
- session hydration

That is enough to persist complete chat history and mid-term memory without prematurely committing
to the long-term graph/vector architecture.
