# Functional coverage of the venue book

**Purpose:** identify the behaviour the functional specification already covers,
the behaviour it does not yet cover, and the evidence needed before writing more
requirements.

This is a working inventory, not a requirements document. The status below is
based on the current book structure and the project documentation. Before
writing or changing a requirement, check the current implementation and its
tests. The component documents can also be wrong or out of date.

## What the book is for

`book.tex` describes the behaviour required of both `pubsub_itc_fw` and the
venue built on it. It should explain what members, operators, and other
components can rely on. It is not a catalogue of classes, configuration keys,
or implementation details; those belong in the component and framework
documentation.

The requirements must distinguish three things:

- **Built behaviour:** what the system does now. Requirements about it should
  have tests that verify them.
- **Required but not built behaviour:** what the system must do but does not
  yet. State the requirement, describe the gap, and cite an open bug where one
  records that gap.
- **Out of scope or deliberately absent behaviour:** what this venue does not
  provide, such as matching orders. State this clearly so a reader does not
  mistake an absence for an unfinished requirement.

Do not turn every implementation detail into a requirement. A requirement
should state an observable promise or a necessary constraint, explain why it
exists, and say what verifies it or that it remains uncovered.

## Current shape of the book

The book already gives high availability substantial coverage. Its framework
chapter also has a detailed pub/sub section. The remaining chapters are at
different stages:

| Book chapter | Current coverage | Main work still needed |
|---|---|---|
| The framework | Reactor, threading, inter-thread communication, memory, IPC, serialisation, timers, WAL, configuration and logging, and general pub/sub behaviour | Check that each public framework guarantee is stated at the right level and has coverage. Keep implementation descriptions in the framework documentation. |
| Applications | A partial set of order-gateway checks, throttles, publisher placeholders, and a technical-events discussion | Specify ordinary venue behaviour across the complete order and report path, along with the services that admit members and administer their access. |
| Building, configuring, releasing and deploying | A new chapter describes the current build, release, deployment, TOML templates, and environment profiles; it is written but not audited | Check each statement against the scripts and environment files. Decide whether the functional specification needs observable requirements for release identification, installation, rollback, and process management. |
| Security | Chapter placeholder | State the security outcomes required of member connections, internal links, credentials, administrative access, and stored secrets. Decide which detail belongs here and which belongs in operations documentation. |
| High availability | Detailed requirements for process and machine failures, sequencers, engines, gateways, publishers, leases, and recovery | Preserve the existing detail while checking its assumptions against current behaviour. Avoid duplicating ordinary operation requirements when adding failure cases. |
| Metrics | Chapter placeholder | Specify what the venue measures and exposes, who can collect it, and what decisions the measurements must support. Distinguish measurements from correctness guarantees. |
| Disaster recovery | Settled positions and open questions, not a complete requirement set | Decide and specify the required site-loss procedure, recovery objectives, uncertain order and credential state, and rehearsal evidence. The current text says its figures are not measured. |

This is a structure-level assessment. It does not mean every requirement in a
substantial chapter has been rechecked against current code.

## Coverage by capability

| Capability | What the book covers now | Work to do |
|---|---|---|
| Member order entry | Some validation, refusal, cancellation fields, and throttling requirements appear in the Applications chapter. High-availability chapters cover many failure cases. | Define the normal order journey and the complete outcomes visible to a member. Include accepted, refused, duplicate, unavailable, and malformed requests, and say which protocol differences are intentional. |
| Order and book behaviour | The high-availability material covers recovery and preservation of open orders. The venue deliberately does not match orders. | State the ordinary order lifecycle and book rules independently of failover: identity, accepted fields, what rests, how cancellation works, terminal outcomes, capacity, and what is explicitly not implemented. Check the proposed rules against engine behaviour. |
| FIX gateway and session | The book has selected FIX requirements, while the separate FIX documents describe parsing, session numbering, resend, and responses in more detail. | Decide which FIX session promises are contractual: logon and logout, heartbeat and timeout, sequence-number handling, resend and gap fill, duplicate handling, and reconnect behaviour. Avoid duplicating protocol definitions that can be referenced. |
| Binary gateway and session | The book requires consistency between gateways in some places. The component document describes the binary protocol and known differences. | Specify the member-visible binary session, authentication, command and report outcomes, reconnect behaviour, and intentional differences from FIX. State any missing features, such as report resend, as gaps if they are required. |
| Authentication and member access | Authentication and credential management are described in component and operations documents; the Security chapter is empty. | Define who may log on, how credential and permission changes take effect, what happens when authentication is unavailable, and what a member is told on failure. Include lock, disable, password-change, and gateway-provisioning outcomes. |
| Administration | The Admin Service document describes firm and comp ID management, roles, passwords, and live credential updates. The book has no clear functional account of the administrative service. | Specify authorised administrative actions, validation and refusal, effect timing, partial failure across authentication instances, and the recovery path when durable and in-memory state differ. |
| Sequencing and reports | The HA chapters cover replication, report preservation, routing across leader changes, and session state. The sequencer document describes ordinary command and report processing. | Specify the normal service guarantees: command order, when a command is considered taken, report generation and routing, numbering, duplicate suppression, and what the member can infer from each reply. Link to failure requirements rather than restating them. |
| Trading state and halts | High availability includes halt-related cases and there is a technical-events section. The component documentation describes trading phases and their propagation. | Settle the behaviour required for opening, accepting orders, halting, announcing state, and resuming. Separate the authoritative decision from how each component learns about it. Include what a member sees and what happens to open orders. |
| Pub/sub | The framework chapter has general topic, ordering, resume, retention, and HA requirements. The matching engine publisher and order activity recorder portions in the Applications chapter are placeholders or unsettled. | State the built publisher's topics, record contents, ordering, access, resume, and failure behaviour. Keep the unbuilt recorder and external bus integration clearly marked as requirements or designs not yet implemented. |
| Durable state and restart | The book has WAL, checkpoint, and extensive HA recovery requirements. | Ensure the ordinary restart contract is complete for each component, including empty or damaged state, retained order state, sequence numbers, and the boundary between process restart and machine loss. |
| Security and transport | The Security chapter is empty; secure communications are described elsewhere. | Set requirements for encryption, peer identity, credential confidentiality, administration-channel access, certificate failure and rotation, and data at rest where needed. Confirm whether every deployed protocol has the required protection. |
| Metrics and operational signals | The Metrics chapter is empty; the operations documents describe existing Prometheus metrics and logging. | State which operational conditions need to be detectable, what measures are required, and what must remain optional. Do not use a metric or asynchronous log as proof of a correctness property. |
| Deployment and configuration | Framework configuration and operations documents describe configuration and deployment. The book has only scattered configuration-related requirements. | Specify only externally important outcomes: invalid configuration is rejected clearly, required components and endpoints are known, startup dependencies are handled, and a deployment can be stopped and started safely. Keep key-by-key reference material elsewhere. |
| Performance and capacity | Some limits and performance-related statements appear in gateway and recovery material. | Separate measurable service objectives from correctness requirements. For each figure, record its workload, measurement method, and whether it is a target, a limit, or an observation. Do not present an unmeasured estimate as a guarantee. |

The separate component documents to consult include the
[architecture map](../orientation/architecture.md), the [venue component
index](../venue/README.md), the [FIX index](../fix/README.md), the
[framework index](../framework/README.md), and the [pub/sub
index](../pubsub/README.md).

## Recommended order of work

### 1. Agree the book's boundary

Use the title and purpose already in `book.tex`: this is a functional
specification for the framework and the venue built on it. Keep member,
operator, and component promises here. Keep class structure, message layouts,
ports, and configuration reference tables in their existing technical
documents unless one of them expresses a necessary behaviour.

Record the deliberate boundaries early. The current venue accepts and cancels
orders but does not match them; it is built around one instrument; and the
order activity recorder and external bus path are not yet built. Verify these
statements against the current source before relying on them.

### 2. Complete ordinary operation before adding more failure cases

The existing book is strongest on HA, while normal application behaviour is
incomplete. Expand the Applications chapter first, following one command from
the member through gateway checks, authentication and access decisions,
sequencing, the order book, report creation, and the reply to the member.
Describe cancellation and session behaviour alongside that path.

This provides a stable ordinary-operation contract for the HA requirements to
extend. It also makes it easier to see which failure cases need distinct
requirements and which are already covered by normal outcomes.

### 3. Write access and trading-state requirements

After the order path, cover authentication, administrative changes, and the
trading state that controls whether commands may be accepted. These decisions
affect both gateways and several internal components, so define who owns each
decision, how it reaches the components that must act on it, and what the
member observes.

### 4. Specify deployment and operational promises

Check the build, configuration, release, and deployment chapter against
verified project behaviour. Then write Security and Metrics from verified
deployment behaviour and explicit operational needs. Complete pub/sub for the
publisher that exists, while keeping planned recorder and external transport
requirements visibly unimplemented. Finish disaster recovery after its open
consequences have decisions and measurable recovery objectives.

### 5. Check requirement coverage as each area is written

For every requirement:

- give it one permanent identifier from `requirement_ids.txt`;
- explain the consequence it prevents;
- link existing HA scenarios or TLA+ counterexamples where they verify it;
- use `\uncovered{...}` when there is no suitable verification yet; and
- use a gap only for a difference the venue still has, citing an open bug when
  one records it.

Do not claim a scenario verifies a requirement just because it exercises the
same component. Check what the scenario actually asserts. Do not use a closed
bug to describe a current gap.

## Evidence to gather before drafting requirements

For each capability area, use the same evidence sequence:

1. Read the corresponding current component and design documents, noting
   statements that are explicitly planned or not implemented.
2. Trace the relevant entry point and state changes in source code. Confirm
   behaviour that is surprising, safety-critical, or disputed rather than
   inferring it from names or comments.
3. Find the tests that assert the behaviour and read the assertion itself.
   Distinguish a client-visible assertion from a log-marker check or a test
   that only observes an intermediate action.
4. Compare the implementation with the intended product behaviour. Record
   the difference as a requirement gap instead of rewriting the requirement
   to match the code.
5. Add requirements in a coherent area, build the book to emit `book.req`,
   and run `scripts/check_book_requirements.py`. Run the check in strict mode
   when the surrounding requirements and tests are ready to account for all
   scenarios.

## First writing unit

The first substantive unit should be the normal order lifecycle in
`applications/applications.tex`, not a broad rewrite of the HA chapters:

1. Identify a new order and the essential member/session identity it carries.
2. State the gateway's validation and refusal outcomes for both protocols.
3. State when the sequencer has taken the command and what ordering the member
   can rely on.
4. State how the matching engine accepts or refuses it and what remains on
   the book.
5. State how each outcome is reported to the member, including cancellation.
6. List the unimplemented functions and unresolved behaviour separately.

Before writing those requirements, verify each item against current source,
existing tests, `docs/venue/`, `docs/fix/`, and the open entries in
`docs/bug_list.md`. Allocate requirement identifiers only after the wording
and scope are clear.
