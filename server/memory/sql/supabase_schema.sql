create table if not exists public.memory_sessions (
    session_id text primary key,
    user_id text not null,
    system_prompt text not null,
    created_at timestamptz not null,
    ended_at timestamptz
);

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

alter table public.conversation_items
    drop constraint if exists conversation_items_episode_id_fkey;

alter table public.conversation_items
    add constraint conversation_items_episode_id_fkey
    foreign key (episode_id)
    references public.mid_term_episodes(episode_id);

create index if not exists conversation_items_session_order_idx
    on public.conversation_items (session_id, item_index);

create index if not exists conversation_messages_session_item_order_idx
    on public.conversation_messages (session_id, item_index, message_index);

create index if not exists mid_term_episodes_session_created_at_idx
    on public.mid_term_episodes (session_id, created_at);
