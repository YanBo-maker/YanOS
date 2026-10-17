"""Explicit official SDK 2.3.0 acceptance; no SDK dependency in production/default CTest."""
import asyncio
import hashlib
from importlib.metadata import version, PackageNotFoundError
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
from semantic_fixture import image, note
from semantic_json_checks import logical_hash


async def await_pid(path):
    deadline=time.monotonic()+8
    while not path.exists():
        if time.monotonic()>=deadline:
            raise RuntimeError('SDK fixture runner failed to start')
        await asyncio.sleep(.02)
    return int(path.read_text())


async def await_gone(pid):
    deadline=time.monotonic()+10
    while Path('/proc',str(pid)).exists():
        if time.monotonic()>=deadline:
            raise RuntimeError('SDK cancellation/EOF failed to reap runner')
        await asyncio.sleep(.02)


async def run(server, options, source_hash, root):
    from mcp import Client
    from mcp.client.stdio import StdioServerParameters
    for mode in ('2026-07-28', 'legacy', 'auto'):
        target = StdioServerParameters(command=str(server), args=options)
        async with Client(target, mode=mode, read_timeout_seconds=15) as client:
            expected = '2025-11-25' if mode == 'legacy' else '2026-07-28'
            assert client.protocol_version == expected, ('actual_protocol_version', mode, client.protocol_version)
            listing = await client.list_tools()
            assert [t.name for t in listing.tools] == ['yanos_search', 'yanos_index_status'], 'sdk_discovery'
            status = (await client.call_tool('yanos_index_status')).model_dump(by_alias=True, exclude_none=True)
            assert status['isError'] is False and status['structuredContent']['state'] == 'READY', 'sdk_status'
            result = (await client.call_tool('yanos_search', {'query': '设备如何叫醒处理器'})).model_dump(by_alias=True, exclude_none=True)
            assert result['isError'] is False, ('sdk_search_failed', result)
            value = result['structuredContent']
            assert value['source_sha256'] == source_hash and value['shown'] > 0, 'sdk_actual_evidence'
            assert json.loads(result['content'][0]['text']) == value, 'sdk_compatible_content'
            failure = (await client.call_tool('yanos_search', {'query': ''})).model_dump(by_alias=True, exclude_none=True)
            assert failure['isError'] is True and 'structuredContent' not in failure, 'sdk_error_not_empty_success'
            assert json.loads(failure['content'][0]['text']) == {'code': 'INVALID'}, 'sdk_error_token'
            print('PASS official mcp==2.3.0 stdio', mode, 'actual=' + client.protocol_version,
                  'rows=' + str(value['shown']))
        pidfile=root/('sdk-pid-'+mode)
        target=StdioServerParameters(command=str(server),args=options,
            env=dict(os.environ,YAN_TEST_EMBED_DELAY='2',YAN_TEST_SERVER_PID_FILE=str(pidfile)))
        async with Client(target,mode=mode,read_timeout_seconds=15) as client:
            task=asyncio.create_task(client.call_tool('yanos_search',{'query':'cpu'}))
            pid=await await_pid(pidfile)
            task.cancel()
            try:
                await task
                raise AssertionError('SDK cancellation unexpectedly returned a result')
            except asyncio.CancelledError:
                pass
            await await_gone(pid)
            deadline=time.monotonic()+10
            while True:
                r=(await client.call_tool('yanos_index_status')).model_dump(by_alias=True,exclude_none=True)
                if not r['isError']:
                    assert r['structuredContent']['state']=='READY', 'sdk_after_cancel_status'
                    break
                assert json.loads(r['content'][0]['text'])=={'code':'BUSY'}, 'sdk_cancel_failure_boundary'
                if time.monotonic()>=deadline:
                    raise RuntimeError('SDK cancelled task slot not reclaimed')
                await asyncio.sleep(.02)
            pidfile.unlink()
            eof_task=asyncio.create_task(client.call_tool('yanos_search',{'query':'cpu'}))
            eof_pid=await await_pid(pidfile)
        abandoned=await asyncio.gather(eof_task,return_exceptions=True)
        assert isinstance(abandoned[0],BaseException), 'sdk_close_did_not_publish_success'
        await await_gone(eof_pid)
        print('PASS official mcp==2.3.0 stdio',mode,'cancel+reuse+active-close/reap')


def main():
    server, semantic, runner = map(lambda p: Path(p).resolve(), sys.argv[1:])
    for p in (server, semantic, runner):
        if not p.is_file():
            raise RuntimeError('FAIL implementation/fixture missing: ' + str(p))
    try:
        sdk = version('mcp')
    except PackageNotFoundError:
        print('SKIP external official MCP SDK missing', file=sys.stderr)
        return 77
    if sdk != '2.3.0':
        raise RuntimeError('expected pinned mcp==2.3.0, found ' + sdk)
    with tempfile.TemporaryDirectory(prefix='yan-mcp-sdk-') as tmp:
        root = Path(tmp)
        disk, index, model = [root/n for n in ('source', 'index', 'model')]
        files = [('irq.txt', note('外设中断', '外设经PLIC通知CPU处理中断。UART收到字符后唤醒等待任务。\r\n')),
                 ('uart.txt', b'CPU wakes a waiting task when a device interrupt arrives.\n')]
        disk.write_bytes(image(files)); model.write_text('universal')
        options = ['--disk-image', str(disk), '--index', str(index), '--model', str(model),
                   '--runner', str(runner), '--startup-timeout', '3', '--request-timeout', '3']
        p = subprocess.run([str(semantic), 'rebuild', *options], capture_output=True, timeout=15)
        if p.returncode:
            raise RuntimeError(('SDK fixture rebuild failed', p.stderr))
        before = [hashlib.sha256(p.read_bytes()).digest() for p in (disk, index, model, runner)]
        asyncio.run(run(server, options, logical_hash(files), root))
        assert before == [hashlib.sha256(p.read_bytes()).digest() for p in (disk, index, model, runner)], 'sdk_read_only'
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
