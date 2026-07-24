# Pi adapter protocol

`PiRuntime` does not invoke an agent that can execute commands by itself. It starts `PI_COMMAND` with the JSON arguments from `PI_COMMAND_ARGS_JSON` and communicates via newline-delimited JSON on standard input/output.

This boundary is deliberate: every command requested by Pi must pass through `CommandPolicy`, `ShellExecutor`, command audit records, output limits, timeouts, and cancellation.

## Messages from runtime to adapter

```json
{ "type": "task", "taskId": "uuid", "prompt": "check nginx", "cwd": "/srv/app" }
```

`cwd` is supplied by the process working directory and is not currently repeated in the JSON message.

## Messages from adapter to runtime

Ask the runtime to execute a command:

```json
{ "type": "exec", "id": "call-1", "command": "systemctl is-active nginx" }
```

The runtime responds on stdin:

```json
{
  "type": "exec_result",
  "id": "call-1",
  "result": { "status": "succeeded", "exitCode": 0, "stdout": "active\n", "stderr": "" }
}
```

When complete, emit exactly one final message:

```json
{ "type": "final", "report": "nginx is active; no remediation was required." }
```

To fail explicitly, emit `{"type":"error","message":"reason"}`. Any non-JSON output on stdout is a protocol error; use stderr for diagnostics.
