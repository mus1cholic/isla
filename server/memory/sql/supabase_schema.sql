-- =============================================================================
-- Working Memory + Mid-Term Memory
-- =============================================================================

-- One row per runtime session.
create table if not exists public.memory_sessions (
    session_id text primary key,
    user_id text not null,
    system_prompt text not null,
    created_at timestamptz not null,
    ended_at timestamptz
);

-- Snapshot of current working-memory state per user.
create table if not exists public.user_working_memory (
    user_id text primary key,
    session_id text not null,
    working_memory jsonb not null,
    rendered_working_memory text not null,
    updated_at timestamptz not null
);

-- Ordered conversation timeline items (ongoing episodes or episode stubs).
create table if not exists public.conversation_items (
    session_id text not null references public.memory_sessions(session_id) on delete cascade,
    item_index bigint not null,
    item_type text not null check (item_type in ('ongoing_episode', 'episode_stub')),
    episode_id text,
    episode_stub_content text,
    episode_stub_created_at timestamptz,
    check (
        (
            item_type = 'ongoing_episode'
            and episode_id is null
            and episode_stub_content is null
            and episode_stub_created_at is null
        ) or (
            item_type = 'episode_stub'
            and episode_id is not null
            and episode_stub_content is not null
            and episode_stub_created_at is not null
        )
    ),
    primary key (session_id, item_index)
);

-- Raw messages inside ongoing episode items.
create table if not exists public.conversation_messages (
    session_id text not null,
    item_index bigint not null,
    message_index bigint not null,
    turn_id text not null,
    role text not null check (role in ('user', 'assistant')),
    content text not null,
    created_at timestamptz not null,
    primary key (session_id, item_index, message_index),
    foreign key (session_id, item_index)
        references public.conversation_items(session_id, item_index)
        on delete cascade
);

-- Flushed mid-term episodes with tiered compaction summaries.
create table if not exists public.mid_term_episodes (
    episode_id text primary key,
    session_id text not null references public.memory_sessions(session_id) on delete cascade,
    source_item_index bigint not null,
    tier1_detail text,
    tier2_summary text not null,
    tier3_ref text not null,
    tier3_keywords text[] not null default '{}',
    salience integer not null check (salience between 1 and 10),
    embedding jsonb not null default '[]'::jsonb,
    created_at timestamptz not null
);

-- Deferred constraint alterations for cross-table foreign keys.

alter table public.conversation_items
    drop constraint if exists conversation_items_episode_id_fkey;

alter table public.conversation_items
    add constraint conversation_items_episode_id_fkey
    foreign key (episode_id)
    references public.mid_term_episodes(episode_id);

alter table public.conversation_messages
    drop constraint if exists conversation_messages_session_id_item_index_fkey;

alter table public.conversation_messages
    add constraint conversation_messages_session_id_item_index_fkey
    foreign key (session_id, item_index)
    references public.conversation_items(session_id, item_index)
    on delete cascade
    deferrable initially immediate;

alter table public.user_working_memory
    drop constraint if exists user_working_memory_session_id_fkey;

alter table public.user_working_memory
    add constraint user_working_memory_session_id_fkey
    foreign key (session_id)
    references public.memory_sessions(session_id)
    on delete cascade;

-- RPC functions for multi-row memory mutations.

create or replace function public.split_conversation_item_with_episode_stub(
    p_session_id text,
    p_conversation_item_index bigint,
    p_episode_id text,
    p_episode_stub_content text,
    p_episode_stub_created_at timestamptz,
    p_remaining_messages jsonb
)
returns void
language plpgsql
as $$
declare
    v_remaining_count bigint;
    v_target_message_count bigint;
    v_split_at_message_index bigint;
    v_item_shift_base bigint;
    v_matching_suffix_count bigint;
begin
    set constraints all deferred;

    if not exists (
        select 1
        from public.conversation_items
        where session_id = p_session_id
          and item_index = p_conversation_item_index
          and item_type = 'ongoing_episode'
    ) then
        raise exception using
            errcode = '23514',
            message = 'split flush target must be an ongoing episode';
    end if;

    v_remaining_count := jsonb_array_length(p_remaining_messages);
    if v_remaining_count < 1 then
        raise exception using
            errcode = '23514',
            message = 'remaining_ongoing_episode must contain at least one message';
    end if;

    select count(*)
    into v_target_message_count
    from public.conversation_messages
    where session_id = p_session_id
      and item_index = p_conversation_item_index;

    if v_target_message_count < v_remaining_count then
        raise exception using
            errcode = '23514',
            message = 'remaining_ongoing_episode does not match persisted conversation item';
    end if;

    v_split_at_message_index := v_target_message_count - v_remaining_count;
    if v_split_at_message_index < 2 then
        raise exception using
            errcode = '23514',
            message = 'split flush requires at least two completed messages before the remaining suffix';
    end if;

    select count(*)
    into v_matching_suffix_count
    from jsonb_array_elements(p_remaining_messages) with ordinality as expected(message, ordinality)
    join public.conversation_messages persisted
      on persisted.session_id = p_session_id
     and persisted.item_index = p_conversation_item_index
     and persisted.message_index = v_split_at_message_index + expected.ordinality - 1
    where persisted.role = expected.message ->> 'role'
      and persisted.content = expected.message ->> 'content'
      and persisted.created_at = ((expected.message ->> 'created_at')::timestamptz);

    if v_matching_suffix_count <> v_remaining_count then
        raise exception using
            errcode = '23514',
            message = 'remaining_ongoing_episode does not match persisted conversation messages';
    end if;

    select coalesce(max(item_index), p_conversation_item_index) + 1
    into v_item_shift_base
    from public.conversation_items
    where session_id = p_session_id;

    update public.conversation_items
    set item_index = item_index + v_item_shift_base
    where session_id = p_session_id
      and item_index > p_conversation_item_index;

    update public.conversation_messages
    set item_index = item_index + v_item_shift_base
    where session_id = p_session_id
      and item_index > p_conversation_item_index;

    insert into public.conversation_items(
        session_id,
        item_index,
        item_type,
        episode_id,
        episode_stub_content,
        episode_stub_created_at
    ) values (
        p_session_id,
        p_conversation_item_index + 1,
        'ongoing_episode',
        null,
        null,
        null
    );

    update public.conversation_items
    set item_index = item_index - v_item_shift_base + 1
    where session_id = p_session_id
      and item_index > p_conversation_item_index + v_item_shift_base;

    update public.conversation_messages
    set item_index = item_index - v_item_shift_base + 1
    where session_id = p_session_id
      and item_index > p_conversation_item_index + v_item_shift_base;

    update public.conversation_messages
    set item_index = p_conversation_item_index + 1,
        message_index = message_index - v_split_at_message_index
    where session_id = p_session_id
      and item_index = p_conversation_item_index
      and message_index >= v_split_at_message_index;

    update public.conversation_items
    set item_type = 'episode_stub',
        episode_id = p_episode_id,
        episode_stub_content = p_episode_stub_content,
        episode_stub_created_at = p_episode_stub_created_at
    where session_id = p_session_id
      and item_index = p_conversation_item_index;
end;
$$;

create or replace function public.clear_session_working_set(
    p_session_id text
)
returns void
language plpgsql
as $$
begin
    -- NOTICE: This is a full working-set reset. Deleting conversation_items also deletes the
    -- matching conversation_messages rows via the ON DELETE CASCADE foreign key, so the persisted
    -- raw transcript for the session is removed here as well.
    delete from public.conversation_items
    where session_id = p_session_id;

    delete from public.mid_term_episodes
    where session_id = p_session_id;
end;
$$;

-- Working memory + mid-term memory indexes.

create index if not exists conversation_items_session_order_idx
    on public.conversation_items (session_id, item_index);

create index if not exists conversation_messages_session_item_order_idx
    on public.conversation_messages (session_id, item_index, message_index);

create index if not exists mid_term_episodes_session_created_at_idx
    on public.mid_term_episodes (session_id, created_at);

create index if not exists user_working_memory_session_id_idx
    on public.user_working_memory (session_id);

-- =============================================================================
-- Long-Term Memory: Knowledge Graph + Episodic Vector Store
-- =============================================================================

-- Embedding model: gemini-embedding-2-preview with output_dimensionality=1536.
-- Native output is 3072 dims but pgvector HNSW indexes cap at 2000 for the vector type,
-- so we request half-dimensionality from the API.
create extension if not exists vector with schema extensions;

-- Entities: first-class nodes in the Knowledge Graph.
create table if not exists public.entities (
    entity_id           text primary key,
    user_id             text not null,
    label               text not null,
    category            text not null,
    activeness          integer not null default 1
                        check (activeness between 1 and 10),
    active_model_text   text,
    familiar_label_text text,
    name_embedding      extensions.vector(1536),
    created_at          timestamptz not null,
    updated_at          timestamptz not null,
    unique (entity_id, user_id)
);

-- Relationships: enriched edges connecting two entities.
create table if not exists public.relationships (
    relationship_id     text primary key,
    user_id             text not null,
    from_entity_id      text not null,
    predicate           text not null,
    to_entity_id        text not null,
    foreign key (from_entity_id, user_id) references public.entities(entity_id, user_id),
    foreign key (to_entity_id, user_id) references public.entities(entity_id, user_id),
    weight              double precision not null default 0.0,
    observation_count   integer not null default 0,
    last_observed_at    timestamptz not null,
    source_episode_ids  text[] not null default '{}',
    embedding           extensions.vector(1536),
    is_archived         boolean not null default false,
    archived_at         timestamptz,
    superseded_by       text references public.relationships(relationship_id),
    created_at          timestamptz not null
);

-- Long-term episodes: consolidated episodic memories.
create table if not exists public.long_term_episodes (
    lte_id                  text primary key,
    user_id                 text not null,
    summary_full            text,
    summary_compressed      text not null,
    keywords                text[] not null default '{}',
    embedding               extensions.vector(1536),
    outcome                 text not null default 'informational'
                            check (outcome in ('resolved', 'abandoned', 'ongoing', 'informational')),
    complexity              integer not null default 1
                            check (complexity between 1 and 10),
    original_episode_ids    text[] not null default '{}',
    caused_by               text references public.long_term_episodes(lte_id),
    led_to                  text references public.long_term_episodes(lte_id),
    created_at              timestamptz not null
);

-- Junction table: bidirectional cross-links between long-term episodes and entities.
create table if not exists public.long_term_episode_entities (
    lte_id      text not null references public.long_term_episodes(lte_id) on delete cascade,
    entity_id   text not null references public.entities(entity_id) on delete cascade,
    primary key (lte_id, entity_id)
);

-- Entity indexes.
create index if not exists entities_user_id_idx
    on public.entities (user_id);
create index if not exists entities_user_activeness_idx
    on public.entities (user_id, activeness);

create index if not exists relationships_user_id_idx
    on public.relationships (user_id);

-- Relationship indexes (partial: exclude archived edges from active lookups).
create index if not exists relationships_from_entity_active_idx
    on public.relationships (from_entity_id) where not is_archived;
create index if not exists relationships_to_entity_active_idx
    on public.relationships (to_entity_id) where not is_archived;

-- Vector similarity indexes (HNSW for better recall).
create index if not exists entities_name_embedding_idx
    on public.entities using hnsw (name_embedding extensions.vector_cosine_ops);
create index if not exists relationships_embedding_idx
    on public.relationships using hnsw (embedding extensions.vector_cosine_ops);
create index if not exists long_term_episodes_embedding_idx
    on public.long_term_episodes using hnsw (embedding extensions.vector_cosine_ops);

-- Episode-entity cross-link reverse index.
create index if not exists long_term_episode_entities_entity_idx
    on public.long_term_episode_entities (entity_id);

-- Long-term episode lookups.
create index if not exists long_term_episodes_user_id_idx
    on public.long_term_episodes (user_id);
