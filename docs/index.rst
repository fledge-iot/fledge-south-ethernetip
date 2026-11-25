EtherNet/IP South Plugin
========================

Detailed documentation for the Fledge south plugin that reads and writes Allen-Bradley / Rockwell PLC data over EtherNet/IP.

--------------

Table of contents
-----------------

1.  :ref:`Overview <ethernetip-1-overview>`
2.  :ref:`Requirements and installation <ethernetip-2-requirements-and-installation>`
3.  :ref:`How the plugin works <ethernetip-3-how-the-plugin-works>`
4.  :ref:`Configuration reference <ethernetip-4-configuration-reference>`
5.  :ref:`Tag map format <ethernetip-5-tag-map-format>`
6.  :ref:`Data types <ethernetip-6-data-types>` · :ref:`Declared-type verification <ethernetip-61-declared-type-verification>`
7.  :ref:`Reading structure <ethernetip-7-reading-structure>`
8.  :ref:`Writing to the PLC (control) <ethernetip-8-writing-to-the-plc-control>`
9.  :ref:`Logging and diagnostics <ethernetip-9-logging-and-diagnostics>`
10. :ref:`Troubleshooting <ethernetip-10-troubleshooting>`
11. :ref:`Behavioural notes and limits <ethernetip-11-behavioural-notes-and-limits>`
12. :ref:`Complete example configurations <ethernetip-12-complete-example-configurations>`

--------------

.. _ethernetip-1-overview:

1. Overview
-----------

``fledge-south-ethernetip`` is a C++ Fledge south plugin. It wraps the
`libplctag <https://github.com/libplctag/libplctag>`__ library (2.7.x) and turns PLC tag
values into Fledge ``Reading`` objects. It also implements ``plugin_write``, so configured
tags can be written from Fledge control pipelines.

+---------------------------------+--------------------------------------------+
| Property                        | Value                                      |
+=================================+============================================+
| Plugin name                     | ``ethernetip``                             |
+---------------------------------+--------------------------------------------+
| Plugin type                     | South                                      |
+---------------------------------+--------------------------------------------+
| Mode                            | Asynchronous (``SP_ASYNC``) with control   |
|                                 | support (``SP_CONTROL``)                   |
+---------------------------------+--------------------------------------------+
| Plugin version                  | 3.1.0                                      |
+---------------------------------+--------------------------------------------+
| Fledge plugin interface version | 2.0.0                                      |
+---------------------------------+--------------------------------------------+
| Minimum Fledge version          | see ``fledge.version`` (currently          |
|                                 | ``>= 3.1``)                                |
+---------------------------------+--------------------------------------------+
| Shared object                   | ``libethernetip.so``                       |
+---------------------------------+--------------------------------------------+
| Install location                | ``$FLEDGE_ROOT/plugins/south/ethernetip/`` |
+---------------------------------+--------------------------------------------+

Supported PLC families:

- Allen-Bradley ControlLogix / CompactLogix (``controllogix``)
- Allen-Bradley Logix5000 family (``logix5000``)
- Allen-Bradley Micro800 (``micro800``)
- Allen-Bradley MicroLogix (``micrologix``)
- Allen-Bradley SLC 500, including SLC 5/05 (``slc500``)
- Omron NJ/NX (``omron-njnx``)

--------------

.. _ethernetip-2-requirements-and-installation:

2. Requirements and installation
--------------------------------

Runtime dependencies
~~~~~~~~~~~~~~~~~~~~

+------------------------+----------------------+----------------------+
| Library                | Role                 | Link type            |
+========================+======================+======================+
| ``libplctag`` >= 2.7.2 | EtherNet/IP tag I/O  | Dynamic              |
+------------------------+----------------------+----------------------+
| Fledge SDK             | ``ConfigCategory``,  | Dynamic              |
| (``common-lib``, …)    | ``Reading``,         |                      |
|                        | ``Logger``           |                      |
+------------------------+----------------------+----------------------+
| RapidJSON              | Tag map parsing      | Header-only (compile |
|                        |                      | time)                |
+------------------------+----------------------+----------------------+

The Debian package bundles ``libplctag.so.2.7.2`` into ``/usr/local/fledge/lib/``, which is
on Fledge's runtime library search path, so no separate libplctag installation is
required on the target machine.

Build from source
~~~~~~~~~~~~~~~~~

.. code:: bash

   mkdir build && cd build
   cmake -DFLEDGE_ROOT=/path/to/fledge ..
   make
   sudo make install

``FLEDGE_ROOT`` may point either at a Fledge source tree (headers are taken from
``C/common/include``, ``C/services/...``) or at an installed layout (``include/``). The build
produces ``libethernetip.so``. The CI build uses the same options as the other Fledge C++
plugins instead: ``-DFLEDGE_SRC=<fledge source tree>`` for headers and
``-DFLEDGE_INSTALL=<fledge install root>`` for ``make install``.

To build the test suites as well:

.. code:: bash

   cmake -DFLEDGE_ROOT=/path/to/fledge -DBUILD_TESTS=ON ..
   make
   ./tests/unit_tests          # link-time libplctag mock, no PLC needed
   ./tests/plugin_load_tests   # dlopen checks on the built .so
   ./tests/integration_tests   # needs a reachable PLC; see VPLC_* env vars

Debian package
~~~~~~~~~~~~~~

The ``.deb`` is built by the shared Fledge plugin workflow, called from
``.github/workflows/build-deb.yml``. ``requirements.sh`` builds libplctag from source
into ``/usr/local`` when it is not already present.

Creating the service
~~~~~~~~~~~~~~~~~~~~

Add a south service in the Fledge UI (**South -> Add -> ethernetip**), or via the REST
API:

.. code:: bash

   curl -sX POST http://localhost:8081/fledge/service -d '{
     "name": "PLC-Line1",
     "type": "south",
     "plugin": "ethernetip",
     "enabled": true,
     "config": {
       "address": {"value": "192.168.1.10"},
       "plc":     {"value": "controllogix"},
       "path":    {"value": "1,0"},
       "asset":   {"value": "Line1"}
     }
   }'

Individual items can be changed later with:

.. code:: bash

   curl -sX PUT http://localhost:8081/fledge/category/PLC-Line1/pollInterval \
        -d '{"value": "500"}'

Any category change causes Fledge to call ``plugin_reconfigure``, which re-runs the
plugin's ``configure()`` logic described in :ref:`§4.1 <ethernetip-41-how-changes-are-applied>`.

--------------

.. _ethernetip-3-how-the-plugin-works:

3. How the plugin works
-----------------------

.. _ethernetip-31-threads:

3.1 Threads
~~~~~~~~~~~

The plugin runs three kinds of thread:

+---------------------+-----------------------+-----------------------+
| Thread              | Created by            | Purpose               |
+=====================+=======================+=======================+
| Poll thread         | ``plugin_start``      | Runs the read cycle   |
|                     |                       | every                 |
|                     |                       | ``pollInterval`` ms   |
|                     |                       | and ingests readings  |
+---------------------+-----------------------+-----------------------+
| Tag creation thread | first poll after      | Issues                |
|                     | ``configure()``       | ``plc_tag_create``    |
|                     |                       | for every tag and     |
|                     |                       | waits for the         |
|                     |                       | connection so the     |
|                     |                       | poll loop never       |
|                     |                       | blocks                |
+---------------------+-----------------------+-----------------------+
| libplctag tickler   | libplctag internally  | Drives the protocol   |
|                     |                       | state machine and     |
|                     |                       | completes async       |
|                     |                       | operations            |
+---------------------+-----------------------+-----------------------+

The poll thread holds a per-instance mutex (``PLCTag::m_mutex``) for the duration of each
read cycle. ``plugin_reconfigure`` and ``plugin_write`` take the same mutex, so
reconfiguration and set-point writes are serialised against polling.

.. _ethernetip-32-tag-state-machine:

3.2 Tag state machine
~~~~~~~~~~~~~~~~~~~~~

::

   configure()                  ->  NEEDS_CREATION
   createTagsBackground() ends   ->  READY | PARTIAL | CREATION_FAILED
   doCleanUp() (reconfigure)     ->  NEEDS_CREATION

- **NEEDS_CREATION** - the next poll launches the background creation thread and returns
  no readings.
- **PARTIAL** - some tags were created, some failed. Polling proceeds with the tags that
  exist.
- **CREATION_FAILED** - no read tag could be created at all. The plugin logs a
  ``[CONN_ERR]`` message (debounced to once every 300 polls) and flips back to
  ``NEEDS_CREATION`` so creation is retried on the next poll.
- **READY** - every configured read tag was created. A configuration with only write
  tags and an empty ``readmap`` also reaches ``READY``.

Tag creation waits up to ``timeout`` ms for all tags to leave ``PENDING``. Tags still pending
at the deadline are aborted, destroyed and marked failed; they are retried on the next
creation cycle.

.. _ethernetip-33-read-cycle:

3.3 Read cycle
~~~~~~~~~~~~~~

**Scatter-gather mode** (default):

1. **Scatter** - for each tag, check ``plc_tag_status()``; tags still ``PENDING`` are skipped
   for this cycle. All other tags get a non-blocking ``plc_tag_read(handle, 0)``.
2. **Gather** - poll each tag's status until it completes or the ``timeout`` deadline
   expires. Successful tags are marked as holding fresh data; failed and timed-out tags
   are aborted (which clears libplctag's in-flight state) and counted as failures.
3. **Collect** - build ``Reading`` objects from the tags that actually succeeded. A tag
   whose read failed is *not* emitted, so stale buffer contents are never published as
   fresh values.

**Sequential mode**: each tag is read with a blocking ``plc_tag_read(handle, timeout)``,
one after another. Failures are aborted and counted the same way.

.. _ethernetip-34-per-tag-failure-recovery:

3.4 Per-tag failure recovery
~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Each read tag has a consecutive-failure counter. Any failed read - a rejected read
request, an error status, or a gather timeout - increments it; any success resets it to
zero. After **5 consecutive failed polls** the tag is aborted, destroyed and recreated;
if even the create call fails, the plugin falls back to a full creation cycle. This
threshold is currently a compile-time constant (``m_maxTagFailures`` in ``include/plctag.h``)
and is not exposed as a configuration item.

Recovery is logged as:

::

   [EtherIP|Line1|192.168.1.10] [TAG_RECOVER] tag='Pump01_FlowRate' failed 5 consecutive polls (PLCTAG_ERR_BUSY) - recreating tag

.. _ethernetip-35-poll-timing:

3.5 Poll timing
~~~~~~~~~~~~~~~

The poll thread computes its next deadline *after* the read cycle finishes, so the real
cycle time is ``pollInterval + read duration``. The sleep is broken into 50 ms slices so
that ``plugin_shutdown`` is prompt; as a result the effective interval is quantised to
roughly 50 ms.

--------------

.. _ethernetip-4-configuration-reference:

4. Configuration reference
--------------------------

All items appear in the Fledge UI in the order below.

+-------+-----------------------+---------------+-------------+-----------------------+
| Order | Item                  | UI name       | Type        | Default               |
+=======+=======================+===============+=============+=======================+
| 1     | ``asset``             | Asset Name    | string      | ``PLCTags``           |
+-------+-----------------------+---------------+-------------+-----------------------+
| 2     | ``address``           | PLC Address   | string      | *(empty)*             |
+-------+-----------------------+---------------+-------------+-----------------------+
| 3     | ``plc``               | PLC Type      | enumeration | ``controllogix``      |
+-------+-----------------------+---------------+-------------+-----------------------+
| 4     | ``path``              | CIP Path      | string      | ``1,0``               |
+-------+-----------------------+---------------+-------------+-----------------------+
| 5     | ``timeout``           | Timeout (ms)  | string      | ``5000``              |
+-------+-----------------------+---------------+-------------+-----------------------+
| 6     | ``pollInterval``      | PLC Poll Loop | string      | ``1000``              |
|       |                       | Interval (ms) |             |                       |
+-------+-----------------------+---------------+-------------+-----------------------+
| 7     | ``readmap``           | Read Tag Map  | JSON        | 10 example tags       |
+-------+-----------------------+---------------+-------------+-----------------------+
| 8     | ``writemap``          | Write Tag Map | JSON        | ``{"tags":[]}``       |
|       |                       |               |             | (empty)               |
+-------+-----------------------+---------------+-------------+-----------------------+
| 9     | ``readingStructure``  | Reading       | enumeration | ``Reading per asset`` |
|       |                       | Structure     |             |                       |
+-------+-----------------------+---------------+-------------+-----------------------+
| 10    | ``readMode``          | Read Mode     | enumeration | ``scatter-gather``    |
+-------+-----------------------+---------------+-------------+-----------------------+
| 11    | ``debugLevel``        | Debug Level   | enumeration | ``1``                 |
+-------+-----------------------+---------------+-------------+-----------------------+
| 12    | ``statsInterval``     | Stats         | string      | ``100``               |
|       |                       | Interval      |             |                       |
+-------+-----------------------+---------------+-------------+-----------------------+
| 13    | ``outageLogInterval`` | Outage Log    | string      | ``3600``              |
|       |                       | Interval (s)  |             |                       |
+-------+-----------------------+---------------+-------------+-----------------------+

.. _ethernetip-41-how-changes-are-applied:

4.1 How changes are applied
~~~~~~~~~~~~~~~~~~~~~~~~~~~

``configure()`` runs on every ``plugin_init`` and every ``plugin_reconfigure``. It first
compares the new ``address``, ``plc``, ``path``, ``readmap`` and ``writemap`` against the current
values. If any of those changed **to a non-empty value**, the plugin performs a full
cleanup: the background creation thread is cancelled, every tag handle is destroyed, the
tag lists are cleared, statistics and the poll counter are reset, and the state returns
to ``NEEDS_CREATION``. The next poll rebuilds every tag and the "PLUGIN CONNECTED" banner
is logged again.

The remaining items (``asset``, ``timeout``, ``pollInterval``, ``readingStructure``, ``readMode``,
``debugLevel``, ``statsInterval``) are applied in place with no reconnect.

Note that an **empty** new value never triggers a rebuild, and the two groups of items
behave differently when cleared:

- Clearing ``address``, ``plc`` or ``path`` to an empty string is ignored entirely - the
  previous value is kept and nothing is rebuilt.
- Clearing ``readmap`` or ``writemap`` stores the empty string but does **not** tear down the
  tags that already exist, so the service keeps polling the previously configured tags
  until some other change forces a rebuild. To genuinely stop reading a tag, remove that
  entry from the map rather than emptying the whole map.

.. _ethernetip-42-asset---asset-name:

4.2 ``asset`` - Asset Name
~~~~~~~~~~~~~~~~~~~~~~~~~~

- **Type:** string · **Default:** ``PLCTags``
- **Applied:** immediately, without reconnecting.

The default Fledge asset name for emitted readings. It is used for any read tag that does
not carry its own ``asset`` override in the readmap (:ref:`§5.2 <ethernetip-52-per-tag-asset-and-datapoint-remapping>`).

The asset name is also part of every log line prefix (``[EtherIP|<asset>|<address>]``), so
giving each service instance a distinct asset name makes multi-PLC logs readable.

An empty value is accepted and will produce readings with an empty asset name; set
something meaningful.

.. _ethernetip-43-address---plc-address:

4.3 ``address`` - PLC Address
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

- **Type:** string · **Default:** empty
- **Applied:** triggers a full tag rebuild when changed to a non-empty value.

IP address or resolvable hostname of the PLC or of the EtherNet/IP gateway module that
fronts it. It becomes the ``gateway=`` attribute of every libplctag tag string. Do not
include a port; EtherNet/IP is fixed at TCP 44818.

If this is left empty the plugin still starts, but every ``plc_tag_create`` fails and the
service settles into the ``CREATION_FAILED`` retry loop with ``[CONN_ERR]`` messages.

.. _ethernetip-44-plc---plc-type:

4.4 ``plc`` - PLC Type
~~~~~~~~~~~~~~~~~~~~~~

- **Type:** enumeration · **Default:** ``controllogix``
- **Values:** ``controllogix``, ``logix5000``, ``micro800``, ``micrologix``, ``slc500``, ``omron-njnx``
- **Applied:** triggers a full tag rebuild when changed.

Passed straight through as the libplctag ``plc=`` attribute, with two special cases:

- **``micro800``** - the CIP ``path`` is omitted entirely (Micro800 controllers have no
  backplane routing), and the string attributes ``str_count_word_bytes=1`` and
  ``str_is_counted=1`` are added so ``STRING`` tags decode correctly.
- **``controllogix``** - enables the BOOL array index rewrite described in
  :ref:`§5.4 <ethernetip-54-controllogix-bool-arrays>`. Other PLC types do not get this rewrite.

``slc500`` and ``micrologix`` are the same PCCC code path inside libplctag - both resolve to
the same tag setup, address parser and byte order, so either value drives an SLC 500 or a
MicroLogix identically. They are listed separately only so the configured type matches the
hardware on site. Both require the CIP ``path`` to be left blank
(:ref:`§4.5 <ethernetip-45-path---cip-path>`).

Resulting tag string prefixes:

::

   # controllogix / logix5000 / omron-njnx
   protocol=ab-eip&gateway=<address>&path=<path>&plc=<plc>&name=<tag>

   # slc500 / micrologix (no path)
   protocol=ab-eip&gateway=<address>&plc=<plc>&name=<tag>

   # micro800
   protocol=ab-eip&gateway=<address>&plc=Micro800&str_count_word_bytes=1&str_is_counted=1&name=<tag>

.. _ethernetip-45-path---cip-path:

4.5 ``path`` - CIP Path
~~~~~~~~~~~~~~~~~~~~~~~

- **Type:** string · **Default:** ``1,0``
- **Applied:** triggers a full tag rebuild when changed, including when cleared.

The CIP routing path from the Ethernet port to the CPU. All spaces are stripped from the
value before use, so ``1, 0`` and ``1,0`` are equivalent.

An empty value is meaningful, not "unset": when the field is blank the ``path=`` attribute is
omitted from the tag string entirely. This is required for ``slc500`` and ``micrologix``, which
have no backplane to route across - libplctag warns ``A path is not supported for this PLC type if it is not for a DH+ bridge`` and the session fails to reach the CPU. Because the
shipped default is ``1,0``, an SLC 500 or MicroLogix service must have this field cleared.

Common values:

+------------------------+---------------------------------------------+
| Path                   | Meaning                                     |
+========================+=============================================+
| ``1,0``                | Backplane (port 1), slot 0 - the usual      |
|                        | ControlLogix/CompactLogix case              |
+------------------------+---------------------------------------------+
| ``1,3``                | Backplane, slot 3 - CPU in slot 3 of the    |
|                        | chassis                                     |
+------------------------+---------------------------------------------+
| ``2,192.168.1.10,1,0`` | Out of an ENBT port to another chassis,     |
|                        | then backplane slot 0                       |
+------------------------+---------------------------------------------+
| *(blank)*              | SLC 500 / MicroLogix on their own Ethernet  |
|                        | port or a 1761-NET-ENI                      |
+------------------------+---------------------------------------------+
| *(ignored)*            | Micro800 - the value is not used at all     |
+------------------------+---------------------------------------------+

A wrong path is the most common cause of tags that never leave ``PENDING`` and are aborted
at the creation deadline.

.. _ethernetip-46-timeout---timeout-ms:

4.6 ``timeout`` - Timeout (ms)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

- **Type:** string holding an integer · **Default:** ``5000``
- **Valid range:** 1 - 120000 ms
- **Applied:** immediately, without reconnecting.

Validation is strict: the value must parse completely as a decimal integer. Trailing
characters (``"5000ms"``), a value of ``0``, or anything above 120000 is rejected with a
``[CONFIG_ERR]`` warning and the previous timeout is kept.

This single value drives four different deadlines:

+-----------------------------+----------------------------------------+
| Where                       | Effect                                 |
+=============================+========================================+
| Tag creation                | How long ``createTagsBackground``      |
|                             | waits for all tags to connect before   |
|                             | aborting the stragglers                |
+-----------------------------+----------------------------------------+
| Scatter-gather gather phase | Deadline for the whole batch of reads  |
|                             | in one poll cycle                      |
+-----------------------------+----------------------------------------+
| Sequential mode             | Per-tag blocking read timeout - worst  |
|                             | case ``timeout × tag count`` per cycle |
+-----------------------------+----------------------------------------+
| Writes                      | Timeout passed to ``plc_tag_write``    |
+-----------------------------+----------------------------------------+

Because the gather deadline applies to the whole poll cycle, ``timeout`` should be
comfortably below ``pollInterval`` in scatter-gather mode; otherwise a bad cycle can
overrun the poll period. A typical LAN read completes in tens of milliseconds, so
1000–5000 ms is a generous setting.

.. _ethernetip-47-pollinterval---plc-poll-loop-interval-ms:

4.7 ``pollInterval`` - PLC Poll Loop Interval (ms)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

- **Type:** string holding an integer · **Default:** ``1000``
- **Applied:** immediately, without reconnecting.

Delay inserted between the end of one read cycle and the start of the next. Parsed with
``atoi``; only values greater than zero are accepted, and anything non-numeric parses as 0
and is therefore ignored (the previous value is kept, silently). There is no upper bound.

Guidance:

- The actual cycle time is ``pollInterval`` plus the time the reads take, and the sleep is
  quantised to ~50 ms.
- Driving this below ~100 ms puts real load on the PLC; ControlLogix CPUs will start
  rejecting or delaying requests long before the plugin does.
- If your poll interval is shorter than the time reads actually take, cycles will simply
  run back to back.

.. _ethernetip-48-readmap---read-tag-map:

4.8 ``readmap`` - Read Tag Map
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

- **Type:** JSON · **Default:** ten example tags
- **Applied:** triggers a full tag rebuild when changed.

A JSON **object** with a ``tags`` array listing the PLC tags to read. Full syntax,
validation rules and remapping options are in :ref:`§5 <ethernetip-5-tag-map-format>`.

An unparseable value produces a ``[CONFIG_ERR]`` log line and results in no read tags; with
no read tags the plugin still reaches ``READY`` and can be used purely for writes. An
*empty* value is a special case - see the caveat in :ref:`§4.1 <ethernetip-41-how-changes-are-applied>`.

.. _ethernetip-49-writemap---write-tag-map:

4.9 ``writemap`` - Write Tag Map
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

- **Type:** JSON · **Default:** ``{"tags":[]}`` - empty, so a new instance is read-only
  until write tags are declared.
- **Applied:** triggers a full tag rebuild when changed.

Same object-with-``tags``-array shape as ``readmap``, but only ``name`` and ``type`` are honoured

- ``asset`` and ``datapoint`` are meaningless for writes and are ignored.

Write tags get their own libplctag handles, separate from read handles, even when the
same PLC tag appears in both maps. They are created **after** the read tags, using a
blocking create with the configured ``timeout``.

Only tags listed here can be written. A control request naming anything else is rejected
with a warning and ``plugin_write`` returns ``false``.

.. _ethernetip-410-readingstructure---reading-structure:

4.10 ``readingStructure`` - Reading Structure
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

- **Type:** enumeration · **Default:** ``Reading per asset``
- **Values:** ``Reading per asset``, ``Reading per PLC tag``
- **Applied:** immediately, without reconnecting.

Controls how datapoints are packaged into readings; see :ref:`§7 <ethernetip-7-reading-structure>` for
worked examples. The comparison is against those exact strings - any other value leaves
the current setting unchanged.

.. _ethernetip-411-readmode---read-mode:

4.11 ``readMode`` - Read Mode
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

- **Type:** enumeration · **Default:** ``scatter-gather``
- **Values:** ``scatter-gather``, ``sequential``
- **Applied:** immediately, without reconnecting.

Only the literal string ``sequential`` selects sequential mode; every other value (including
typos) selects scatter-gather.

+------------------+-------------------------+-------------------------+
|                  | scatter-gather          | sequential              |
+==================+=========================+=========================+
| Requests         | All tags issued at      | One tag at a time,      |
|                  | once, then awaited      | blocking                |
+------------------+-------------------------+-------------------------+
| Worst-case cycle | ~\ ``timeout`` for the  | ``timeout`` × number of |
|                  | whole batch             | tags                    |
+------------------+-------------------------+-------------------------+
| Slow/bad tag     | Times out individually, | Stalls the rest of the  |
|                  | others unaffected       | cycle                   |
+------------------+-------------------------+-------------------------+
| PLC load         | Higher instantaneous    | Gentle, one outstanding |
|                  | request burst           | request                 |
+------------------+-------------------------+-------------------------+

Use scatter-gather unless a device misbehaves with concurrent requests; sequential is the
safe fallback for older MicroLogix controllers and heavily loaded gateways.

.. _ethernetip-412-debuglevel---debug-level:

4.12 ``debugLevel`` - Debug Level
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

- **Type:** enumeration · **Default:** ``1``
- **Values:** ``0``, ``1``, ``2``, ``3``
- **Applied:** immediately, without reconnecting.

Gates the plugin's ``debug``-level log lines. Values outside 0–3 are ignored. Note that
these messages are emitted at Fledge's ``debug`` level, so the service's own log level must
also be set to *debug* for them to appear.

+-------+--------------------------------------------------------------+
| Level | Adds                                                         |
+=======+==============================================================+
| 0     | Nothing - only info/warning/error messages                   |
+-------+--------------------------------------------------------------+
| 1     | Poll start/complete summaries, scatter/gather counts, tag    |
|       | creation progress                                            |
+-------+--------------------------------------------------------------+
| 2     | Every tag value read, per-tag read durations                 |
+-------+--------------------------------------------------------------+
| 3     | Full trace: tag strings, BOOL array rewrites                 |
+-------+--------------------------------------------------------------+

Levels 2 and 3 log at least one line per tag per poll - useful for commissioning, far too
noisy for production.

Separately, libplctag's own internal log output is forwarded into the Fledge log with a
``LIBPLCTAG tag=<id>:`` prefix, mapped by libplctag severity (error -> error, warn -> warn,
info -> info, detail/spew -> debug).

.. _ethernetip-413-statsinterval---stats-interval:

4.13 ``statsInterval`` - Stats Interval
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

- **Type:** string holding an integer · **Default:** ``100``
- **Applied:** immediately, without reconnecting.

Number of poll cycles between performance summary lines. Parsed with ``atoi``; only values
greater than zero are accepted. After each summary the accumulators are reset, so every
line describes only the window since the previous one. See
:ref:`§9.3 <ethernetip-93-performance-statistics>` for the format.

At the default 1 s poll interval, ``100`` yields a summary roughly every 100 seconds.

.. _ethernetip-414-outageloginterval---outage-log-interval:

4.14 ``outageLogInterval`` - Outage Log Interval
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

- **Type:** string holding an integer, seconds · **Default:** ``3600``
- **Range:** 0–86400. Out-of-range values are rejected with a ``[CONFIG_ERR]`` warning and
  the previous setting is kept.
- **Applied:** immediately, without reconnecting.

Minimum interval between repeat ``[OUTAGE]`` reports while PLC tags are unreachable. ``0``
logs every occurrence, which is useful when
reproducing a fault but produces thousands of lines an hour during a real outage.

An outage is reported three times regardless of this setting: once on the leading edge,
once per interval while it persists (carrying the elapsed duration, the wall-clock start,
and a count of suppressed occurrences), and once on recovery with the total downtime.
Occurrences between reports are counted, not dropped silently.

Aggregation applies only when **every** tag is failing - the signature of a connection or
power fault, where each tag would otherwise repeat the same message. A partial failure is
reported per tag, since which tag is broken is the entire content of that event. Per-tag
detail during a full outage stays available at ``debugLevel`` 1 and above.

Independently of the logging, tag creation backs off between retries from 1 s to a 30 s
ceiling (``RETRY_BACKOFF_MIN_MS`` / ``RETRY_BACKOFF_MAX_MS``), so a cabinet down overnight no
longer draws a create attempt every ~7 s. The backoff resets as soon as any tag is
created, and retries are never abandoned.

--------------

.. _ethernetip-5-tag-map-format:

5. Tag map format
-----------------

.. _ethernetip-51-structure:

5.1 Structure
~~~~~~~~~~~~~

Both maps are JSON objects containing a ``tags`` array:

.. code:: json

   {
     "tags": [
       { "name": "MotorSpeed",   "type": "DINT" },
       { "name": "PumpPressure", "type": "REAL" },
       { "name": "AlarmFlag",    "type": "BOOL" },
       { "name": "BatchName",    "type": "STRING" }
     ]
   }

The top-level object wrapper is required - Fledge ``JSON`` configuration items must be JSON
objects, not bare arrays.

Per-entry rules:

+---------------+----------+--------------+--------------------------------------+
| Field         | Required | Applies to   | Notes                                |
+===============+==========+==============+======================================+
| ``name``      | yes      | both maps    | The PLC tag name, exactly as the     |
|               |          |              | controller knows it                  |
+---------------+----------+--------------+--------------------------------------+
| ``type``      | yes      | both maps    | One of the types in                  |
|               |          |              | §6                    ;              |
|               |          |              | case-sensitive                       |
+---------------+----------+--------------+--------------------------------------+
| ``program``   | no       | both maps    | The Logix program that owns the tag; |
|               |          |              | omit for controller scope            |
|               |          |              | (§5.5                              ) |
+---------------+----------+--------------+--------------------------------------+
| ``asset``     | no       | readmap only | Overrides the global ``asset`` for   |
|               |          |              | this tag                             |
+---------------+----------+--------------+--------------------------------------+
| ``datapoint`` | no       | readmap only | Overrides the datapoint name         |
|               |          |              | (default: the PLC tag address)       |
+---------------+----------+--------------+--------------------------------------+

Validation is per entry, and bad entries are skipped rather than failing the whole map:

- An element that is not a JSON object -> warning, skipped.
- Missing or non-string ``name`` -> warning, skipped.
- ``program`` present but empty or not a string -> warning, skipped.
- ``program`` present and ``name`` already starting with ``Program:`` -> warning, skipped.
- Missing or non-string ``type`` -> warning, skipped.
- Unrecognised ``type`` -> warning, skipped.
- The whole document failing to parse, not being an object, or lacking a ``tags`` array ->
  one ``[CONFIG_ERR]`` error and no tags at all from that map.

``name`` may address array elements and structure members using the usual libplctag syntax,
for example ``MyArray[4]`` or ``MyUDT.Member``. The complete
tag string (prefix plus name) must stay under 500 characters; longer strings are rejected
per tag with a ``[CONFIG_ERR]``.

.. _ethernetip-52-per-tag-asset-and-datapoint-remapping:

5.2 Per-tag asset and datapoint remapping
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Read entries may be remapped onto arbitrary Fledge asset and datapoint names:

.. code:: json

   {
     "tags": [
       { "name": "Tank1_Level",       "type": "REAL", "asset": "Tank1", "datapoint": "Level" },
       { "name": "Tank1_Temperature", "type": "REAL", "asset": "Tank1", "datapoint": "Temperature" },
       { "name": "Pump1_Running",     "type": "BOOL", "datapoint": "Running" },
       { "name": "Counter1",          "type": "DINT" }
     ]
   }

- ``asset`` omitted -> the global ``asset`` value is used.
- ``datapoint`` omitted -> the PLC tag address is used verbatim, including the
  ``Program:<program>.`` prefix for a program-scoped tag.

This works with both reading structures. Under ``Reading per asset``, datapoints are grouped
by their *effective* asset name, so the first two entries above collapse into a single
``Tank1`` reading with ``Level`` and ``Temperature`` datapoints. Asset ordering and datapoint
ordering follow declaration order in the map.

.. _ethernetip-53-duplicate-names:

5.3 Duplicate names
~~~~~~~~~~~~~~~~~~~

Nothing deduplicates the map. Listing the same PLC tag twice creates two independent
libplctag handles and issues two reads per cycle. In ``Reading per PLC tag`` mode that
yields two readings; in ``Reading per asset`` mode, two datapoints with the same name in one
reading. Use ``datapoint`` overrides if you genuinely need the same PLC tag under two names.

.. _ethernetip-54-controllogix-bool-arrays:

5.4 ControlLogix BOOL arrays
~~~~~~~~~~~~~~~~~~~~~~~~~~~~

ControlLogix stores BOOL arrays packed into 32-bit words, so ``MyBits[33]`` is not directly
addressable. When ``plc`` is ``controllogix`` and the entry type is ``BOOL``, the plugin
rewrites the name automatically:

::

   MyBits[33]  ->  MyBits[1].1        (33 / 32 = 1, 33 % 32 = 1)
   MyBits[0]   ->  MyBits[0].0
   MyBits[31]  ->  MyBits[0].31
   MyBits[32]  ->  MyBits[1].0

Write the natural index in the readmap and let the plugin do the arithmetic. A negative
index is rejected with a ``[CONFIG_ERR]`` and the tag is skipped. The rewrite is applied
only for ``controllogix``; on other PLC types the name is passed through unchanged.

.. _ethernetip-55-program-scoped-tags:

5.5 Program-scoped tags
~~~~~~~~~~~~~~~~~~~~~~~

Logix tags are either controller-scoped or owned by a program. An entry names the program
in its ``program`` field. An entry with ``program`` is program-scoped and an entry without it
is controller-scoped:

.. code:: json

   {
     "tags": [
       { "name": "Level",     "type": "REAL", "program": "MainProgram" },
       { "name": "Bits[33]",  "type": "BOOL", "program": "MainProgram" },
       { "name": "LineSpeed", "type": "REAL" }
     ]
   }

The plugin builds the libplctag address ``Program:<program>.<name>`` while parsing the map,
so the entries above address ``Program:MainProgram.Level``, ``Program:MainProgram.Bits[1].1``
(after the BOOL array rewrite in :ref:`§5.4 <ethernetip-54-controllogix-bool-arrays>`) and ``LineSpeed``.
From then on that address *is* the tag's identity:

- The default datapoint name is the full address, so two programs that each own a ``Level``
  do not collide in one reading. Set ``datapoint`` for a shorter name.
- A ``writemap`` tag is written by its full address: ``plugin_write("Program:MainProgram.SetPoint", …)``.
- Log messages show the full address.

Don't put the ``Program:`` prefix in ``name`` when ``program`` is set. The program would then be
named twice, so the entry is skipped with a warning. Older maps that spell out
``Program:MainProgram.Level`` in ``name`` with no ``program`` field still work, because the name
is passed to libplctag unchanged.

--------------

.. _ethernetip-6-data-types:

6. Data types
-------------

+-------------+------------------+-------------------------+------------------+
| Config type | Aliases          | Read via                | Emitted Fledge   |
|             |                  |                         | datapoint        |
+=============+==================+=========================+==================+
| ``BOOL``    |                  | ``plc_tag_get_bit``     | Integer ``0`` /  |
|             |                  |                         | ``1``            |
+-------------+------------------+-------------------------+------------------+
| ``SINT``    | ``INT8``         | ``plc_tag_get_int8``    | Integer          |
+-------------+------------------+-------------------------+------------------+
| ``USINT``   | ``UINT8``        | ``plc_tag_get_uint8``   | Integer          |
+-------------+------------------+-------------------------+------------------+
| ``INT``     | ``INT16``        | ``plc_tag_get_int16``   | Integer          |
+-------------+------------------+-------------------------+------------------+
| ``UINT``    | ``UINT16``       | ``plc_tag_get_uint16``  | Integer          |
+-------------+------------------+-------------------------+------------------+
| ``DINT``    | ``INT32``        | ``plc_tag_get_int32``   | Integer          |
+-------------+------------------+-------------------------+------------------+
| ``UDINT``   | ``UINT32``,      | ``plc_tag_get_uint32``  | **Float**        |
|             | ``DWORD``        |                         |                  |
+-------------+------------------+-------------------------+------------------+
| ``LINT``    | ``INT64``        | ``plc_tag_get_int64``   | **Float**        |
+-------------+------------------+-------------------------+------------------+
| ``ULINT``   | ``UINT64``       | ``plc_tag_get_uint64``  | **Float**        |
+-------------+------------------+-------------------------+------------------+
| ``REAL``    | ``FLOAT32``      | ``plc_tag_get_float32`` | Float            |
+-------------+------------------+-------------------------+------------------+
| ``LREAL``   | ``FLOAT64``      | ``plc_tag_get_float64`` | Float            |
+-------------+------------------+-------------------------+------------------+
| ``STRING``  |                  | ``plc_tag_get_string``  | String           |
+-------------+------------------+-------------------------+------------------+

Type names are matched case-sensitively and must be upper case.

Points to be aware of:

- **BOOL** arrives as an integer ``0``/``1``, not a JSON boolean.
- **32- and 64-bit unsigned types and ``LINT`` are emitted as floating point**, because
  Fledge integer datapoints are signed 64-bit. Values beyond 2^53 therefore lose
  precision. If you need exact 64-bit counters, read them as ``STRING`` from the PLC or
  split them in PLC logic.
- **STRING** reads into a 256-byte buffer; longer PLC strings are truncated. A failed
  string read logs ``[DATA_ERR]`` and the datapoint is omitted from that cycle.
- Type is *asserted*, not discovered - libplctag reinterprets the raw bytes according to
  the declared type and never errors on a mismatch. The plugin checks the declaration
  against the controller at startup and warns; see
  :ref:`§6.1 <ethernetip-61-declared-type-verification>`.

.. _ethernetip-61-declared-type-verification:

6.1 Declared-type verification
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Because a wrong ``type`` produces plausible-looking numbers rather than an error, the
plugin verifies every tag against the controller once creation completes. libplctag
exposes the CIP data type the PLC reports for a tag, and the plugin compares that with
the declared type, logging a warning per mismatch:

::

   [EtherIP|Line1|192.168.1.10] [TYPE_MISMATCH] tag='MotorSpeed' is declared as REAL but the
     PLC reports INT (CIP type 0x00C3) - values for this tag will be misinterpreted
   [EtherIP|Line1|192.168.1.10] typeCheck: 1 tag(s) have a declared type that does not match
     the PLC - fix the 'type' field in the readmap; readings for those tags are not trustworthy

The check is **advisory only**. A mismatched tag is still created and still polled - the
reinterpretation may be deliberate - so nothing changes except that the problem is now
visible. The check runs once per creation cycle, so it costs nothing per poll.

Cases deliberately *not* reported, to keep the warning trustworthy:

+----------------------------------+----------------------------------+
| Situation                        | Why it is not a mismatch         |
+==================================+==================================+
| ``BOOL`` on a bit-addressed tag  | The PLC reports the containing   |
| (``Tag.7``, or a ControlLogix    | word's type (``DINT``,           |
| BOOL array element)              | ``DWORD``, …); the plugin        |
|                                  | recognises the bit syntax and    |
|                                  | its own BOOL-array rewrite       |
+----------------------------------+----------------------------------+
| An integer declared where the    | Identical data, different label  |
| PLC reports the same-width bit   |                                  |
| string (``DINT`` ↔ ``DWORD``,    |                                  |
| ``INT`` ↔ ``WORD``)              |                                  |
+----------------------------------+----------------------------------+
| ``STRING`` against a structure   | String encodings differ per PLC  |
| (``0x02A0``) or a                | family; only a ``STRING``        |
| family-specific string encoding  | pointed at a *numeric* scalar is |
|                                  | reported                         |
+----------------------------------+----------------------------------+
| Any type the plugin has no rule  | Never guessed at                 |
| for                              |                                  |
+----------------------------------+----------------------------------+

Coverage depends on the PLC family, because the underlying libplctag attribute is only
implemented for some:

+----------------------------------+----------------------------------+
| PLC type                         | Verified?                        |
+==================================+==================================+
| ``controllogix``, ``logix5000``  | Yes                              |
+----------------------------------+----------------------------------+
| ``micro800``                     | Yes                              |
+----------------------------------+----------------------------------+
| ``micrologix``, ``slc500``,      | No - libplctag reports no type   |
| ``omron-njnx``                   | information; tags are silently   |
|                                  | skipped by the check             |
+----------------------------------+----------------------------------+

At ``debugLevel`` 2 the plugin logs its verdict for every tag, including the ones it could
not verify:

::

   typeCheck: tag='Tank1_Level' declared REAL, PLC reports REAL (0x00CA) - OK
   typeCheck: tag='AlarmBits[33]' declared BOOL, PLC reports DINT (0x00C4) - OK
   typeCheck: tag='BatchName' declared STRING, PLC reports 0x02A0 (STRUCT/UDT) - cannot verify
   typeCheck: 2 verified, 0 mismatched, 1 unverifiable

Note that signed/unsigned differences of the same width (``DINT`` declared for a ``UDINT``
tag) *are* reported, since they diverge once values exceed the signed range.

--------------

.. _ethernetip-7-reading-structure:

7. Reading structure
--------------------

Given this readmap:

.. code:: json

   {
     "tags": [
       { "name": "Tank1_Level", "type": "REAL", "asset": "Tank1", "datapoint": "Level" },
       { "name": "Tank1_Temp",  "type": "REAL", "asset": "Tank1", "datapoint": "Temperature" },
       { "name": "Counter1",    "type": "DINT" }
     ]
   }

and ``asset`` = ``PLCTags``:

**``Reading per PLC tag``** - one reading per successful tag:

::

   asset "Tank1"    { "Level": 42.5 }
   asset "Tank1"    { "Temperature": 61.2 }
   asset "PLCTags"  { "Counter1": 1187 }

**``Reading per asset``** (default) - one reading per distinct effective asset, carrying all
of that asset's datapoints:

::

   asset "Tank1"    { "Level": 42.5, "Temperature": 61.2 }
   asset "PLCTags"  { "Counter1": 1187 }

In both modes only tags whose read succeeded in that cycle contribute. A tag that timed
out or errored is simply absent from the reading; the plugin never republishes the
previous cycle's value as if it were fresh. In ``Reading per asset`` mode, an asset whose
tags all failed produces no reading at all.

Choose ``Reading per asset`` when you want fewer, wider readings (cheaper storage, easier
correlation between tags sampled in the same cycle); choose ``Reading per PLC tag`` when
downstream filters or north plugins expect one datapoint per asset.

--------------

.. _ethernetip-8-writing-to-the-plc-control:

8. Writing to the PLC (control)
-------------------------------

The plugin declares ``SP_CONTROL`` and implements ``plugin_write(name, value)``. Fledge
invokes it for set-point writes dispatched to this south service - from a control
pipeline, a north control request, or the control API.

Behaviour:

1. ``name`` is looked up in the ``writemap`` by exact PLC tag address. For a program-scoped
   tag that is ``Program:<program>.<name>`` (:ref:`§5.5 <ethernetip-55-program-scoped-tags>`). Not found -> warning,
   returns ``false``.
2. If the tag's handle is not ready yet (tags still being created), the write is refused
   with a warning and returns ``false``.
3. The string ``value`` is converted according to the map's declared ``type``, staged into the
   tag buffer, and written with a blocking ``plc_tag_write(handle, timeout)``.
4. Returns ``true`` on ``PLCTAG_STATUS_OK``; on failure logs ``[TAG_ERR] write: … FAILED`` with
   the decoded libplctag error and returns ``false``.

Value conversion by type:

+----------------------------------------------+----------------------------------+
| Type                                         | Accepted ``value``               |
+==============================================+==================================+
| ``BOOL``                                     | ``true`` or ``1`` -> 1; anything |
|                                              | else -> 0                        |
+----------------------------------------------+----------------------------------+
| ``SINT``/``USINT``/``INT``/``UINT``/``DINT`` | Decimal integer string           |
+----------------------------------------------+----------------------------------+
| ``LINT``/``ULINT``                           | Decimal integer string (64-bit)  |
+----------------------------------------------+----------------------------------+
| ``REAL``/``LREAL``                           | Decimal or exponent-form float   |
|                                              | string                           |
+----------------------------------------------+----------------------------------+
| ``STRING``                                   | Used verbatim                    |
+----------------------------------------------+----------------------------------+

Successful writes are logged at info level with the tag, type, value and elapsed time, so
control actions are auditable in the service log.

   **Caveat - malformed numeric values.** Numeric conversion uses ``std::stoi`` / ``stoul`` /
   ``stof`` / ``stod`` without a surrounding try/catch. A value that is not parseable as the
   declared type (for example ``"abc"`` for a ``DINT``) raises an uncaught exception out of
   ``plugin_write``. Validate set-point values upstream in the control pipeline until this is
   hardened.

The write path takes the same mutex as the poll loop, so a write is serialised against
reads and may wait for the current poll cycle to finish.

--------------

.. _ethernetip-9-logging-and-diagnostics:

9. Logging and diagnostics
--------------------------

.. _ethernetip-91-log-prefix:

9.1 Log prefix
~~~~~~~~~~~~~~

Every line carries an instance context so multiple south services can share a log:

::

   [EtherIP|<asset>|<address>] <message>

.. _ethernetip-92-error-classification-tags:

9.2 Error classification tags
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

+---------------------+------------------------------------------------+
| Tag                 | Meaning                                        |
+=====================+================================================+
| ``[CONFIG_ERR]``    | Bad configuration - empty address, unknown     |
|                     | type, invalid tag name, out-of-range timeout   |
+---------------------+------------------------------------------------+
| ``[CONN_ERR]``      | Connection-level failure - tag creation        |
|                     | failed, PLC unreachable, bad path              |
+---------------------+------------------------------------------------+
| ``[TAG_ERR]``       | Per-tag I/O failure - read or write rejected   |
|                     | or returned an error status                    |
+---------------------+------------------------------------------------+
| ``[TIMEOUT]``       | Operation exceeded its deadline (creation or   |
|                     | read)                                          |
+---------------------+------------------------------------------------+
| ``[DATA_ERR]``      | Value conversion failure, e.g. a string read   |
|                     | that failed                                    |
+---------------------+------------------------------------------------+
| ``[TAG_RECOVER]``   | A tag failed five polls in a row and is being  |
|                     | destroyed and recreated                        |
+---------------------+------------------------------------------------+
| ``[TYPE_MISMATCH]`` | A tag's declared ``type`` does not match what  |
|                     | the PLC reports - see                          |
|                     | §6.1                                           |
+---------------------+------------------------------------------------+

Grepping for the tag in brackets is the fastest way to triage a misbehaving service.

.. _ethernetip-93-performance-statistics:

9.3 Performance statistics
~~~~~~~~~~~~~~~~~~~~~~~~~~

Every ``statsInterval`` polls:

::

   [EtherIP|PLCTags|192.168.1.10] poll stats [last 100 polls]: avgTotal=14.3ms avgScatter=0.1ms \
     avgGather=13.8ms min/maxGather=11.2/48.7ms avgCollect=0.4ms successRate=100.0% timeouts=0

+-------------------+--------------------------------------------------+
| Field             | Meaning                                          |
+===================+==================================================+
| ``avgTotal``      | Mean time for a complete read cycle              |
+-------------------+--------------------------------------------------+
| ``avgScatter``    | Mean time to issue all read requests (should be  |
|                   | sub-millisecond)                                 |
+-------------------+--------------------------------------------------+
| ``avgGather``     | Mean time waiting for responses - the dominant   |
|                   | term, and your real PLC latency                  |
+-------------------+--------------------------------------------------+
| ``min/maxGather`` | Best and worst gather time in the window; a      |
|                   | large spread means a struggling PLC or network   |
+-------------------+--------------------------------------------------+
| ``avgCollect``    | Mean time converting values into readings        |
+-------------------+--------------------------------------------------+
| ``successRate``   | Tags read successfully ÷ tags attempted, over    |
|                   | the window                                       |
+-------------------+--------------------------------------------------+
| ``timeouts``      | Number of cycles in which at least one tag timed |
|                   | out                                              |
+-------------------+--------------------------------------------------+

A ``successRate`` below 100% with a healthy ``avgGather`` usually points at a handful of
specific bad tags; check the ``[TAG_ERR]`` lines to identify them.

.. _ethernetip-94-startup-banner:

9.4 Startup banner
~~~~~~~~~~~~~~~~~~

The first successful creation cycle logs a summary block that is worth capturing when
commissioning:

::

   [EtherIP|Line1|192.168.1.10] ── PLUGIN CONNECTED ──────────────────────────────
   [EtherIP|Line1|192.168.1.10]   PLC type     : controllogix
   [EtherIP|Line1|192.168.1.10]   Address      : 192.168.1.10
   [EtherIP|Line1|192.168.1.10]   CIP path     : 1,0
   [EtherIP|Line1|192.168.1.10]   Asset name   : Line1
   [EtherIP|Line1|192.168.1.10]   Timeout      : 5000 ms
   [EtherIP|Line1|192.168.1.10]   Read mode    : scatter-gather
   [EtherIP|Line1|192.168.1.10]   Read tags    : 42 configured, 42 created OK
   [EtherIP|Line1|192.168.1.10]   Write tags   : 6 configured, 6 created OK
   [EtherIP|Line1|192.168.1.10]   Create time  : 812 ms
   [EtherIP|Line1|192.168.1.10] ──────────────────────────────────────────────────

``Read tags : 42 configured, 39 created OK`` means the service is in ``PARTIAL`` state -
three tags failed and the ``[CONN_ERR]`` lines just above will name them.

--------------

.. _ethernetip-10-troubleshooting:

10. Troubleshooting
-------------------

.. _ethernetip-no-readings-at-all-conn_err--plc-tags-may-be-invalid-or-connection--failed:

No readings at all, ``[CONN_ERR] … PLC tags may be invalid or connection … failed``
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Every tag failed to create. In order of likelihood: wrong ``path`` (slot number), wrong
``address``, PLC not reachable on TCP 44818 (firewall), or wrong ``plc`` type. The message is
debounced to once every 300 polls; creation is retried on every poll regardless.

Confirm reachability from the Fledge host first:

.. code:: bash

   nc -vz <plc-address> 44818

.. _ethernetip-some-tags-missing-conn_err-createtags-tagx--error:

Some tags missing, ``[CONN_ERR] createTags: tag='X' … ERROR``
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Those specific tag names do not exist on the controller or are not externally accessible.
Check spelling, case, program scope (a program-scoped tag needs its ``program`` field, see
:ref:`§5.5 <ethernetip-55-program-scoped-tags>`), and that the tag is not marked as a constant or otherwise restricted.

``PLCTAG_ERR_BUSY`` repeating for the same tags forever
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

libplctag keeps per-tag "operation in flight" state. An asynchronous read that ends in an
error status leaves that flag set unless the tag is explicitly aborted, and a tag left in
that state answers ``PLCTAG_ERR_BUSY`` to every subsequent read - permanently, for the same
fixed set of tags, at exactly one message per poll.

The plugin now aborts every failed read and recreates a tag that fails five polls in a
row, so this state clears itself. If you see it, look **backwards** in the log for the
*first* error on that tag name - the BUSY messages are the aftermath, and that earlier
error is the actual fault to fix.

.. _ethernetip-timeout--tagx-timeout-after-n-ms:

``[TIMEOUT] … tag='X' TIMEOUT after N ms``
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The read did not complete within ``timeout``. Occasional timeouts on a busy CPU are normal;
persistent ones on the same tags suggest the tag is expensive to read (large arrays or
UDTs), or the PLC is saturated. Try raising ``timeout``, lengthening ``pollInterval``,
trimming the tag count, or switching to ``sequential`` mode to reduce the request burst.

Values are wrong but no errors are logged
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Almost always a type mismatch - the map declares a type different from the controller's,
and libplctag reinterprets the raw bytes without complaining. Search the log for
``[TYPE_MISMATCH]``: on Logix and Micro800 the plugin names the offending tags and the type
the PLC actually reports (:ref:`§6.1 <ethernetip-61-declared-type-verification>`). On SLC 500, MicroLogix and Omron
the check cannot run, so verify against the controller's tag database by hand.

Also remember that ``BOOL`` on ControlLogix arrays needs the natural index
(:ref:`§5.4 <ethernetip-54-controllogix-bool-arrays>`).

Micro800 strings come back garbled
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Micro800 string encoding differs from Logix. The plugin sets the required
``str_count_word_bytes`` / ``str_is_counted`` attributes only when ``plc`` is exactly
``micro800`` - check the PLC type is set correctly.

Nothing appears after a configuration change
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Changing ``address``, ``plc``, ``path``, ``readmap`` or ``writemap`` destroys and rebuilds every
tag. Expect one or two empty poll cycles (and up to ``timeout`` ms of creation) before
readings resume; the "PLUGIN CONNECTED" banner marks the end of the rebuild.

--------------

.. _ethernetip-11-behavioural-notes-and-limits:

11. Behavioural notes and limits
--------------------------------

- **Fixed recovery threshold.** The five-consecutive-failure tag recreation threshold is a
  compile-time constant, not a configuration item.
- **Error debounce.** The ``CREATION_FAILED`` message is emitted once per 300 polls; per-tag
  read errors are *not* debounced and appear every cycle.
- **STRING length.** Reads are limited to 255 characters plus a terminator.
- **64-bit precision.** ``LINT``, ``ULINT``, ``UDINT`` and ``DWORD`` become floating-point
  datapoints - see :ref:`§6 <ethernetip-6-data-types>`.
- **No array-in-one-read support.** Each element must be listed individually; the plugin
  always reads offset 0 of each tag handle.
- **``plugin_poll`` is not implemented.** The plugin is asynchronous; if Fledge ever calls
  ``plugin_poll`` it throws, which indicates a framework-level misconfiguration.
- **Write value parsing can throw.** See the caveat in :ref:`§8 <ethernetip-8-writing-to-the-plc-control>`.
- **One connection per service.** Each south service instance talks to one PLC; use
  multiple services for multiple controllers, and give each a distinct ``asset`` so the log
  prefixes stay distinguishable.

--------------

.. _ethernetip-12-complete-example-configurations:

12. Complete example configurations
-----------------------------------

.. _ethernetip-121-controllogix-per-asset-readings:

12.1 ControlLogix, per-asset readings
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. code:: json

   {
     "asset":            { "value": "Line1" },
     "address":          { "value": "192.168.1.10" },
     "plc":              { "value": "controllogix" },
     "path":             { "value": "1,0" },
     "timeout":          { "value": "3000" },
     "pollInterval":     { "value": "1000" },
     "readingStructure": { "value": "Reading per asset" },
     "readMode":         { "value": "scatter-gather" },
     "debugLevel":       { "value": "1" },
     "statsInterval":    { "value": "100" },
     "readmap": {
       "value": "{\"tags\":[{\"name\":\"Tank01_Level\",\"type\":\"REAL\",\"asset\":\"Tank01\",\"datapoint\":\"Level\"},{\"name\":\"Tank01_Temperature\",\"type\":\"REAL\",\"asset\":\"Tank01\",\"datapoint\":\"Temperature\"},{\"name\":\"Pump01_FlowRate\",\"type\":\"REAL\",\"asset\":\"Pump01\",\"datapoint\":\"FlowRate\"},{\"name\":\"Pump01_SpeedSetpoint\",\"type\":\"DINT\"}]}"
     },
     "writemap": {
       "value": "{\"tags\":[{\"name\":\"Tank01_LevelSetpoint\",\"type\":\"REAL\"},{\"name\":\"Pump01_SpeedSetpoint\",\"type\":\"DINT\"}]}"
     }
   }

.. _ethernetip-122-micro800-read-only-sequential:

12.2 Micro800, read-only, sequential
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. code:: json

   {
     "asset":            { "value": "Micro1" },
     "address":          { "value": "192.168.1.42" },
     "plc":              { "value": "micro800" },
     "timeout":          { "value": "2000" },
     "pollInterval":     { "value": "2000" },
     "readingStructure": { "value": "Reading per PLC tag" },
     "readMode":         { "value": "sequential" },
     "readmap": {
       "value": "{\"tags\":[{\"name\":\"MachineState\",\"type\":\"DINT\"},{\"name\":\"CycleCount\",\"type\":\"DINT\"},{\"name\":\"BatchName\",\"type\":\"STRING\"}]}"
     },
     "writemap": { "value": "{\"tags\":[]}" }
   }

Note that ``path`` is absent - Micro800 ignores it.

.. _ethernetip-123-controllogix-bool-array-bits:

12.3 ControlLogix BOOL array bits
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. code:: json

   {
     "readmap": {
       "value": "{\"tags\":[{\"name\":\"AlarmBits[0]\",\"type\":\"BOOL\",\"asset\":\"Alarms\",\"datapoint\":\"HighLevel\"},{\"name\":\"AlarmBits[33]\",\"type\":\"BOOL\",\"asset\":\"Alarms\",\"datapoint\":\"PumpFault\"},{\"name\":\"AlarmBits[64]\",\"type\":\"BOOL\",\"asset\":\"Alarms\",\"datapoint\":\"EStop\"}]}"
     }
   }

The plugin rewrites these to ``AlarmBits[0].0``, ``AlarmBits[1].1`` and ``AlarmBits[2].0``
before creating the tags; with ``Reading per asset`` they arrive as one ``Alarms`` reading
with three integer ``0``/``1`` datapoints.

--------------

See also
--------

- ``README.md`` - quick reference, build and packaging instructions
- `libplctag documentation <https://github.com/libplctag/libplctag/wiki>`__ - tag string
  attributes and error codes
