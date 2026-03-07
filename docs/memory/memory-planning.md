# Cognitive Memory Architecture: Unified Specification WIP

**Project:** Biomimetic AI Memory System for Virtual Desktop Companions

**Architecture:** Tri-Layered (Working, Mid-Term, Long-Term)

---

## 1. Introduction and Executive Summary

This architecture replaces standard Retrieval-Augmented Generation (RAG) with a multi-tiered cognitive pipeline. It mimics the human brain's ability to maintain a sharp, low-latency **Working Memory**, a fast-access **Mid-Term Staging Area**, and a dual-core **Long-Term Neocortex** that separates logical facts (Semantic) from narrative experiences (Episodic).

Agentic solutions exist, but those are usually focused on optimizing for task execution. For an AI assistant to have a more natural-seeming memory, it must start from what it was designed to mimic - humans.

---

## 2. Term Definitions

Session - The entire user’s session, starting from application open to application close

Conversation - The entire conversation (user + assistant)

Episode - an Episode represents a conversation that happens during a Session. One conversation has Messages revolved around the same Topic. Therefore, as soon as the user switches to a different topic, that indicates the start of a new ConversationEvent. ConversationEvents  may contain many conversations on the same topic, because either 1. the context window limit was too big, or

EpisodeStub - a 1-sentence **Semantic Stub** (e.g., `[System: Previously discussed CMake configuration]`), preserving the "train of thought" without the token weight.

## 3. Layer 1: Working Memory (The Prefrontal Cortex)

**Component:** Active LLM Context Window

**Goal:** Immediate task focus and zero-latency interaction.

The context window of the LLM acts as the prefrontal cortex - able to store a small amount of things, but do so extremely well. The prefrontal cortex can only store a few XXXX.

Typical modern LLM context window have a capacity of around ~1m tokens. However, their effectiveness sharply drops at much earlier stages, similar to the prefrontal cortex. Therefore, we choose to hard-cap the context window at 256k tokens of the model's total limit to ensure high performance is maintained.

**The "Rolling Flush":** Either when the threshold is reached, or an Episode is finished and a new Episode is detected, the system performs a **Semantic Boundary Flush**.

- A lightweight LLM (e.g., Gemini Flash) scans backward to find a natural breaking point (end of a thought, completed code block), and marks the start and the end as a chunk. The end is usually
    - In the case of detecting a new Episode, the chunk is the previous Episode
- This chunk is removed from the window, sent to the mid-term memory, and replaced with an EpisodeStub.

The layout of the working memory will be as follows at all times:
```
{system_prompt}

{lifetime_persistent_memory}

{task_persistent_memory}

{conversation}
```

The layout of Conversation will be as follows at all times:

```
{}
```

---

## 4. Layer 2: Mid-Term Memory (The Hippocampus)

**Component:** Temporally Linked Event Database

**Goal:** Fast-write staging area for recent history with pre-calculated resolutions.

FAISS + GraphDB acts as the hippocampus of the LLM. Here, reads must be near-instantaneous, and writes must also be extremely fast.

The structure of the GraphDB is a simple doubly-linked list (prev and next). Each node contains a single Episode, and the nodes are linked together through either prev or next. If node u links node v through "next", that means the Episode encompassed in node u happened before node v, and vice versa.

As soon as an Episode is determined to be pushed to the mid-term memory, an asynchronous background task (e.g. Gemini Flash) performs two steps.

#### Step 1:

The model generates three different resolution compactions for the Episode, in addition to a **Salience Score (1-10)**:

- **High-Res (Tier 1):** Raw text or exact code/commands, with exact numbers stored. (for scores 8-10).
- **Mid-Res (Tier 2):** Multi-sentence (max 4-6) narrative paragraph (for scores 4-7).
- **Low-Res (Tier 3):** Single summary sentence + 5 keywords (for scores 1-3).
- **Epistemic Tagging:** A boolean flag `contains_actionable_fact` is set if the node contains a state change or system fact (e.g., "I switched to SQLite").

#### Step 2:

Send the mid-res paragraph summary to an embedding model, and store the embedding.

Then, the embedding is stored in FAISS, and the other fields are stored in GraphDB.

### Retrieval Methodology

When the memory triager determines that memory needs to be retrieved, we do a vector embedding retrieval from FAISS, and grab two sets:

1. Starting from the k-most (k=3) similar nodes, we do a decaying-walk, grabbing the appropriate salient-scored compaction
2. The k'-most (k'=3) recent nodes, excluding the coverage from #1

After this, we order each result by time, and then send it back to the main llm prompt

---

## 5. Layer 3: Long-Term Memory (The Neocortex)

**Component:** Dual-Database System (Knowledge Graph + Embedded Vector DB)

**Goal:** Permanent, cross-session knowledge and associative recall.

The neocortex is split into two interconnected subsystems: **Semantic Memory** (what is true) and **Episodic Memory** (what happened). These are not isolated silos — they are bidirectionally linked, and episodes gradually dissolve into pure knowledge over time (a process called **semanticization**).

---

### A. Semantic Memory (Knowledge Graph)

**Role:** Stores the "current state of the world" as a domain-agnostic web of concepts and relationships. This includes everything from user preferences, to project facts, to personal information — all in the same graph. The brain doesn't organize knowledge by "project" or "scope" — it stores concepts generically, and context determines which ones activate.

#### Entities (First-Class Nodes)

Everything is an entity — the user, tools, languages, projects, people, preferences, abstract concepts:

```
Entity {
  id:         "entity_cpp",
  label:      "C++",
  category:   "language",       // lightweight tag for disambiguation (not scoping)
  created_at: ...,
  updated_at: ...
}
```

Note: The `category` tag is not a scope — it exists purely for disambiguation (e.g., "Python" the language vs. "Python" the snake) and to help the extraction LLM avoid creating duplicate entities for the same label.

#### Relationships (Enriched Edges)

Relationships connect any entity to any other entity. Each relationship carries metadata beyond the bare triplet:

```
Relationship {
  from:               "entity_user",
  predicate:          "knows",
  to:                 "entity_cpp",

  confidence:         9,                // 1-10, how certain is this fact
  source_episode_id:  "ep_012",         // provenance: which episode produced this
  created_at:         ...,
  supersedes:         null              // ID of the relationship this one replaced
}
```

**Confidence Scoring:** Not all facts are equally certain. The extraction LLM assigns confidence based on how the fact was established:

| Source | Confidence |
| :--- | :--- |
| User explicitly states a fact | 9 – 10 |
| Inferred from user behavior/code | 6 – 8 |
| Speculative / "I'm thinking about..." | 2 – 5 |

**Provenance:** The `source_episode_id` field traces *why* the system believes something. If the system injects a wrong fact, we can trace back to the originating episode. The `supersedes` field creates a history chain for corrections.

#### Predicate Vocabulary (Controlled Ontology)

To prevent inconsistent predicates (e.g., `uses`, `codes_in`, and `prefers_language` all meaning the same thing), the extraction LLM selects from a bounded predicate set. Start small (~20) and expand as needed:

```
Identity:       is_a, name, alias
Relationships:  knows, works_on, owns, created
Preferences:    prefers, dislikes, enjoys
Properties:     uses, built_with, located_in, has_property
State:          currently_doing, status
Causality:      caused_by, leads_to, depends_on
```

These predicates are domain-agnostic — they can describe a person, a project, a pet, or a music preference. The extraction prompt includes this list so the LLM picks from the menu rather than inventing predicates.

#### Conflict Resolution

Uses **confidence-weighted chronological precedence**:

```
if new_fact.confidence >= existing_fact.confidence:
    supersede(existing, new)
elif new_fact.confidence >= 7:
    supersede(existing, new)    // still fairly confident
else:
    flag_for_review(existing, new)    // don't auto-override low-confidence over high-confidence
```

Note: The `supersedes` field preserves history, so overrides can be undone if the user corrects a bad inference.

#### Retrieval: Spreading Activation

The brain doesn't query facts by scope — it activates a concept, and connected concepts light up. Retrieval works the same way:

1. The current conversation context mentions entities (e.g., "isla" and "rendering")
2. The entity lexicon matches these to `entity_isla` and `entity_rendering`
3. From those nodes, **walk outward 1-2 hops** in the knowledge graph
4. Collect all reachable relationships and inject them as system facts

The context naturally filters what's relevant. If the user is talking about their cat, the walk starts from `[Mochi]` and `[cat]`, and project facts never enter the picture. No explicit scoping needed — the graph topology *is* the scope.

---

### B. Episodic Memory (Embedded Vector DB)

**Role:** Stores narrative memories of past experiences, solutions, and problem-solving journeys. While the Knowledge Graph stores *what is true*, Episodic Memory stores *what happened* — including the reasoning, the failed attempts, and the emotional context that bare facts cannot capture.

#### Structured Episode Records

Each long-term episode is a richly structured record, not just a flat embedding:

```
LongTermEpisode {
  id:                     "lte_042",

  // Tiered summaries (pre-computed at consolidation, served based on retrieval distance)
  summary_full:           "Debugged inverted normals in isla renderer. Initially
                           tried flipping the Z axis, which fixed normals but broke
                           shadow mapping. Root cause was winding order mismatch due
                           to left-handed coordinate system...",
  summary_compressed:     "Fixed inverted normals by switching to CCW face culling
                           after failed Z-axis flip attempt.",
  keywords:               ["normals", "CCW", "winding order", "coordinate system", "rendering"],
  embedding:              [0.23, -0.11, ...],   // for similarity search (generated from summary_full)

  // Entity anchors (bidirectional links TO the knowledge graph)
  related_entities:       ["entity_isla", "entity_ccw_culling", "entity_directx12"],

  // Structural metadata
  outcome:                "resolved",           // resolved | abandoned | ongoing | informational
  valence:                "positive",           // positive | negative | neutral
  complexity:             7,                    // 1-10, how involved was this

  // Temporal
  created_at:             ...,
  original_episode_ids:   ["ep_039", "ep_040", "ep_041"],   // source mid-term episodes

  // Causal links (connections to OTHER long-term episodes)
  caused_by:              null,
  led_to:                 "lte_045"
}
```

Note: All three tiers are pre-computed once during the sleep cycle consolidation, not at retrieval time. Retrieval just picks which field to return.

#### Entity Anchors (Bidirectional Cross-Linking)

This is the critical interaction between the two subsystems:

- **Knowledge Graph → Episodes:** Every entity in the KG accumulates a list of `related_episodes`. When the system retrieves the fact `[isla] → (uses) → [CCW_culling]`, it can *also* surface the experiences that produced that knowledge.

- **Episodes → Knowledge Graph:** Each episode's `related_entities` field links back to the KG. When an episode is retrieved via similarity search (e.g., "we had a similar bug before"), the system can pull the current facts for those entities, providing factual context alongside the narrative.

Together, the narrative gives the *story* and the KG gives the *current truth*. The LLM gets both: "here's what happened last time, and here's what we know is true right now."

#### Causal Chains

Some episodes form sequences of attempts. The `caused_by` / `led_to` fields link them:

```
lte_039: "Tried flipping Z axis to fix normals"    → outcome: abandoned
  └─ led_to → lte_040: "Switched to CCW culling"   → outcome: resolved
```

When the LLM retrieves one episode from a chain, it can walk the causal links to get the full problem-solving trajectory — including failed attempts, so it avoids repeating them.

#### Decay Lifecycle (Semanticization)

Over time, episodic memories fade but their factual contributions persist — mirroring how humans forget the *experience* of learning something but retain the *knowledge*. The tiered summaries provide natural degradation breakpoints:

| Stage | Age | What Happens |
| :--- | :--- | :--- |
| **Fresh** | 0-30 days | `summary_full` + `summary_compressed` + `keywords` all available |
| **Faded** | 30-90 days | `summary_full` dropped. Only `summary_compressed` + `keywords` remain |
| **Semanticized** | 90+ days | All summaries deleted. Factual contributions to the KG remain, with `source_episode_id` preserved for provenance |

This keeps the vector DB from growing unboundedly while ensuring that extracted knowledge lives on in the graph. The experience dies; the lesson survives.

#### Retrieval Methodology

Episodic retrieval uses three paths, not just vector similarity:

**Path 1 — Direct Similarity Search:** Query embedding → cosine similarity → top-K episodes. Handles queries like *"we had a similar problem before."*

**Path 2 — Entity-Triggered Recall:** When the semantic memory's spreading activation hits an entity that has `related_episodes`, those episodes are pulled automatically. Handles queries like *"tell me about the isla rendering pipeline"* — the KG entity `entity_isla` already knows which episodes are related, no embedding search needed.

**Path 3 — Causal Chain Expansion (post-retrieval):** After either of the above paths returns an episode, check for `caused_by` / `led_to` links and pull the full chain. This is a post-retrieval expansion step, not a standalone retrieval path.

**Composite Scoring:** When multiple episodes are candidates, rank them beyond raw cosine similarity:

```
score(episode) =
    similarity(query, episode.embedding) * w1
  + recency_decay(episode.created_at)    * w2
  + outcome_boost(episode.outcome)       * w3
  + freshness(episode.decay_stage)       * w4
```

Where:
- `outcome_boost`: Prefer `resolved` episodes over `abandoned` ones — solved problems are more useful than dead ends
- `freshness`: `Fresh` episodes rank higher than `Faded` ones
- `recency_decay`: Gradual time-based decay so recent experiences are preferred, all else being equal

**Distance-Resolution Mapping:** Retrieved episodes are served at different resolutions depending on how they were reached — mirroring the brain's detail gradient where attention is sharpest at the center and fades at the periphery:

| Retrieval Distance | Field Used | Example |
| :--- | :--- | :--- |
| Direct match (similarity search hit) | `summary_full` | Full narrative with reasoning |
| Entity-triggered (1 hop via KG) | `summary_compressed` | One sentence with outcome |
| Causal chain link | `summary_compressed` | One sentence + outcome tag |
| Hop 2+ (fuzzy awareness) | `keywords` only | Just signal that this episode exists |

**Full retrieval flow:**

1. Query arrives, embed it
2. Cosine similarity search → candidate set A
3. Entity-triggered recall from KG → candidate set B
4. Merge A ∪ B, deduplicate
5. Score each candidate with composite formula
6. For top-N results, expand causal chains
7. Select appropriate summary tier based on retrieval distance
8. Return ordered results

Note: The vector search is the fallback, not the primary path. The entity cross-links are the more brain-like retrieval — the brain recalls experiences *through* the concepts involved, not through abstract similarity. The vector search catches cases where the concepts don't match exactly but the *situation* is analogous.


---



## 6. The Sleep Cycle (Consolidation & Pruning)

**Trigger:** 45+ minutes of idle time or scheduled nightly task.

**Process:**

1.  **Replay:** A high-reasoning LLM scans the Mid-Term linked list.

2.  **Consolidation:** * Nodes marked `contains_actionable_fact: true` are extracted into the **Knowledge Graph**.

    * High-salience nodes (Tier 1 & 2) are embedded into the **Vector DB**.

3.  **Synaptic Pruning:** Any node with `salience < 3` AND `fact: false` is permanently deleted. The Mid-Term staging ground is wiped clean once consolidated.



---



## 7. Runtime Retrieval: The "Lazy" Gatekeeper

The system only reaches for memory when cued, avoiding "context pollution" and unnecessary API latency. Basically, a lightweight (gemini flash) triager is called on every turn to ask, for the current user query, whether everything in the context window is sufficient enough to answer the user's query, or whether extra context is needed. If context is enough, the memory retrieval part is skipped. Otherwise:

| Tier | Trigger | Action |

| :--- | :--- | :--- | IGNORE THESE, START FROM 1 below

| **1. Semantic Router** | Local Tiny Vector Search | Matches prompt against an in-memory **Entity Lexicon** (FAISS/HNSW). If similarity >0.85, the backend pulls the specific Graph triplets and injects them as "System Facts." |

| **2. Mid-Term Expansion** | LLM Tool Call | If the LLM sees a **Stub** in its context window that it needs to expand, it calls `expand_mid_term(event_id)`. |

| **3. Cued Recall** | LLM Tool Call | If the LLM recognizes it needs historical narrative context (e.g., "How did we solve this last month?"), it calls `search_past_episodes(query)`. |

1. Mid-term memory: does a tool call to grab the mid-term memory given the prompt

---



## 8. Operational Summary

* **Writing:** Async and non-blocking. The client remains snappy while the backend (both in C++) handles the "Amygdala" pre-calculations.

* **Reading:** Hierarchical (Local fast-pass -> Main LLM reasoning -> Explicit DB fetch).

* **Hierarchy of Truth:** If Mid-Term memory contradicts a Long-Term Fact, the Mid-Term (most recent) data is treated as the ground truth.

---

## 9. Concrete Example: Birthday Party Planning

This example traces a single user query through all memory layers to show how the full system works together.

### Background

The user has been using the assistant for several months. Over that time, they've:
- Mentioned their friend Sarah's birthday is March 20th
- Discussed Sarah's peanut allergy multiple times
- Planned a surprise party for Sarah last year that went poorly (the venue cancelled last minute, they scrambled to host at home, but the cake was a hit)
- Earlier today, discussed potential restaurant options for this year

### The Query

The user types: **"What should I keep in mind for Sarah's party this year?"**

### Step 1: Lazy Gatekeeper

The triager sees the query references "Sarah's party" — context that isn't fully in the working memory window. Verdict: **retrieve**.

### Step 2: All Three Systems Fire in Parallel

**Mid-Term Memory (FAISS + Linked List):**
- FAISS similarity search matches against today's earlier conversation about restaurant options
- Returns the episode at Tier 2 (mid-res): *"Discussed restaurant options for Sarah's birthday. Narrowed down to Italian or Thai. User leaning toward Thai because Sarah mentioned liking it recently."*
- Recency set (k'=3) also grabs two other recent episodes from this week

**Semantic Memory (Knowledge Graph):**
- Entity lexicon matches "Sarah" → `entity_sarah`
- Spreading activation, hop 1:
  ```
  [Sarah] → (birthday) → [March 20]          ← full triplet
  [Sarah] → (has_allergy) → [peanuts]         ← full triplet (confidence: 10)
  [Sarah] → (prefers_cuisine) → [Thai]        ← full triplet (confidence: 7)
  [Sarah] → (is_a) → [friend]                 ← full triplet
  ```
- Hop 2 (entity-label only, compressed):
  ```
  peanuts is connected to: [anaphylaxis, epipen]
  Thai is connected to: [pad_thai, peanut_sauce]   ← this is relevant!
  ```

**Episodic Memory (Vector DB):**
- Direct similarity search finds last year's party episode (lte_089):
  ```
  summary_full: "Planned surprise party for Sarah's 28th birthday.
  Booked a venue downtown but it cancelled 2 days before. Scrambled
  to host at User's apartment instead. Ordered a custom chocolate
  cake from Luna Bakery which Sarah loved. Guest list was about 15
  people. Lesson learned: always have a backup venue."
  → outcome: resolved, valence: mixed, complexity: 7
  ```
- Entity-triggered recall (via `entity_sarah`) finds another episode (lte_072), served compressed:
  ```
  summary_compressed: "Sarah mentioned she's been trying to eat
  healthier and prefers smaller gatherings now."
  ```
- Causal chain: lte_089 has no `led_to`, so no expansion needed

### Step 3: Merge and Inject

All results are merged, scored, and ordered by time. The assistant's context window now contains:

```
{system_prompt}

{lifetime_persistent_memory}

{retrieved_memory}
  KG Facts:
    - Sarah's birthday: March 20
    - Sarah has peanut allergy (confidence: 10)
    - Sarah prefers Thai cuisine (confidence: 7)
    - Nearby concepts: peanuts → [peanut_sauce], Thai → [pad_thai]

  Recent (Mid-Term):
    - [Today] Discussed restaurants, leaning toward Thai

  Past Experiences (Episodic):
    - [Last year] Party planning: venue cancelled, hosted at home,
      chocolate cake from Luna Bakery was a hit. Lesson: have backup venue.
    - [3 months ago] Sarah prefers smaller gatherings now (compressed)

{conversation}
  User: "What should I keep in mind for Sarah's party this year?"
```

### Step 4: The LLM Responds

With all this context, the assistant can give a comprehensive answer:

> "Here are a few things to keep in mind for Sarah's party:
>
> 1. **Her birthday is March 20th** — that's coming up in less than two weeks
> 2. **Peanut allergy** — important since you're leaning toward Thai, and a lot of Thai dishes use peanuts or peanut sauce. Make sure to check with the restaurant about allergen-safe options
> 3. **Keep it small** — she mentioned a few months ago she prefers smaller gatherings now
> 4. **Have a backup plan** — remember last year when the venue fell through? Maybe confirm the restaurant booking a few days early
> 5. **The cake from Luna Bakery was a hit** — could be worth ordering from them again
> 6. You were leaning toward Thai earlier today — want me to help compare the Thai restaurants you were looking at?"

Note how each piece of the response came from a different memory layer:
- Points 1-2: KG facts (semantic memory)
- Point 2 follow-up about peanut sauce: KG hop 2 connection (Thai → peanut_sauce)
- Point 3: Episodic memory, compressed (entity-triggered recall)
- Points 4-5: Episodic memory, full (direct similarity match)
- Point 6: Mid-term memory (today's conversation)

---

## 10. Future Works


### Mid-term Memory

1. The current way of association only captures temporal adjacency - add weighted edges between nodes that share keywords or high embedding similarity
2. Memory reconsolidation - bump salience score when something gets retrieved
