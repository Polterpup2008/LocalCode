"""Synthetic API and answer checks; performance results are a separate gate."""
import argparse
import ast
import json
import operator
from pathlib import Path

from benchmark_runtime import Server, digest, write_json


def evaluate(node, bindings):
    if isinstance(node, ast.Name) and node.id in bindings:
        return bindings[node.id]
    if isinstance(node, ast.Constant) and isinstance(node.value, (int, float)):
        return node.value
    if isinstance(node, ast.BinOp):
        operations = {ast.Add: operator.add, ast.Sub: operator.sub, ast.Mult: operator.mul,
                      ast.FloorDiv: operator.floordiv, ast.Mod: operator.mod}
        if type(node.op) in operations:
            return operations[type(node.op)](evaluate(node.left, bindings), evaluate(node.right, bindings))
    if isinstance(node, ast.UnaryOp) and isinstance(node.op, ast.USub):
        return -evaluate(node.operand, bindings)
    raise ValueError('Generated code is outside the pure arithmetic checker')


def check_code(code, expected):
    module = ast.parse(code)
    if len(module.body) != 1 or not isinstance(module.body[0], ast.FunctionDef):
        return False
    function = module.body[0]
    if len(function.body) != 1 or not isinstance(function.body[0], ast.Return):
        return False
    if [arg.arg for arg in function.args.args] != ['a', 'b'] or function.decorator_list:
        return False
    return all(evaluate(function.body[0].value, {'a': a, 'b': b}) == answer
               for a, b, answer in expected)


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('--server', type=Path, required=True)
    parser.add_argument('--model', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--gpu-layers', type=int, default=999)
    parser.add_argument('--device', help='Explicit runtime device identifier, such as Vulkan1 or CUDA0')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    contract = {'context': 32768, 'gpu_layers': args.gpu_layers, 'threads': 8, 'batch': 512,
                'ubatch': 512, 'cache_k': 'f16', 'cache_v': 'f16',
                'startup_timeout_s': 150, 'request_timeout_s': 120,
                'model_sha256': digest(args.model)}
    if args.device:
        contract['device'] = args.device
    write_json(args.output / 'contract.json', contract)
    checks = []
    with Server(args.server.resolve(), args.model.resolve(), contract, args.output / 'server') as server:
        def record(name, predicate):
            try:
                passed, detail = predicate()
                row = {'name': name, 'passed': bool(passed), 'detail': detail}
            except Exception as error:
                row = {'name': name, 'passed': False, 'error': str(error)}
            checks.append(row)
            write_json(args.output / 'checks.json', checks)
            print(json.dumps(row), flush=True)

        def chat(name, prompt, schema=None, **extra):
            payload = {'model': server.alias, 'messages': [{'role': 'user', 'content': prompt}],
                       'temperature': 0.0, 'seed': 42, 'max_tokens': 256,
                       'chat_template_kwargs': {'enable_thinking': False}, **extra}
            if schema:
                payload['response_format'] = {'type': 'json_schema', 'json_schema':
                                              {'name': 'answer', 'strict': True, 'schema': schema}}
            write_json(args.output / (name + '.request.json'), payload)
            response = server.request('/v1/chat/completions', payload)
            write_json(args.output / (name + '.response.json'), response)
            return response['choices'][0]['message']

        def roundtrip():
            text = 'LocalCode: caf\u00e9, \u8def\u5f84, \U0001f9ae.\n  a += 2\t# exact bytes'
            tokens = server.request('/tokenize', {'content': text, 'add_special': False})['tokens']
            decoded = server.request('/detokenize', {'tokens': tokens})['content']
            return decoded == text, {'tokens': len(tokens)}
        record('unicode-token-roundtrip', roundtrip)
        numeric_schema = {'type': 'object', 'properties': {'answer': {'type': 'integer'}},
                          'required': ['answer'], 'additionalProperties': False}
        for name, question, expected in (
            ('multiply', 'What is 17 multiplied by 19?', 323),
            ('subtract', 'What is 1000 minus 673?', 327),
            ('negative', 'What is -12 plus 7?', -5),
            ('precedence', 'Evaluate 3 + 4 * 5.', 23),
            ('integer-division', 'What is 57 // 8 in Python?', 7),
            ('modulo', 'What is 57 % 8 in Python?', 1),
            ('array-sum', 'Sum this list: [7, -3, 18, 0, -2].', 20),
            ('length', 'How many characters are in the string LocalCode?', 9),
        ):
            def question_check(name=name, question=question, expected=expected):
                message = chat(name, question + ' Return JSON with one integer field named answer.', numeric_schema)
                answer = json.loads(message['content'])
                return answer == {'answer': expected}, {'actual': answer, 'expected': expected}
            record(name, question_check)
        code_schema = {'type': 'object', 'properties': {'code': {'type': 'string'}},
                       'required': ['code'], 'additionalProperties': False}
        for name, description, expected in (
            ('code-add', 'return a plus b', [(2, 3, 5), (-2, 3, 1), (0, 0, 0)]),
            ('code-subtract', 'return a minus b', [(2, 3, -1), (-2, -3, 1), (0, 0, 0)]),
            ('code-weighted', 'return three times a plus five times b', [(2, 3, 21), (-2, 3, 9), (0, 0, 0)]),
        ):
            def code_check(name=name, description=description, expected=expected):
                message = chat(name, 'Write a pure Python function solve(a, b) using one return expression to ' +
                               description + '. Return JSON with the code string; no Markdown.', code_schema)
                code = json.loads(message['content'])['code']
                return check_code(code, expected), {'code': code, 'input_checks': len(expected)}
            record(name, code_check)

        def tool_check():
            function = {'name': 'sum_ints', 'description': 'Add two integers.',
                        'parameters': {'type': 'object', 'properties': {'a': {'type': 'integer'},
                                       'b': {'type': 'integer'}}, 'required': ['a', 'b'],
                                       'additionalProperties': False}}
            message = chat('tool', 'Call sum_ints to add 17 and 19.',
                           tools=[{'type': 'function', 'function': function}],
                           tool_choice={'type': 'function', 'function': {'name': 'sum_ints'}})
            calls = message.get('tool_calls', [])
            arguments = json.loads(calls[0]['function']['arguments']) if len(calls) == 1 else None
            return len(calls) == 1 and calls[0]['function']['name'] == 'sum_ints' and arguments == {'a': 17, 'b': 19}, message
        record('native-tool-arguments', tool_check)
        def stream_check():
            row = server.complete('stream', 'def add(a,b):\n    return', predict=32, stream=True)
            response = row['response']
            return (row['ttft_s'] is not None and bool(response.get('tokens')) and
                    len(response['tokens']) == response['timings']['predicted_n']), {
                        'tokens': len(response.get('tokens', [])), 'ttft_s': row['ttft_s']}
        record('complete-sse-token-accounting', stream_check)
        record('healthy-after-requests', lambda: (server.request('/health')['status'] == 'ok', {}))
    result = {'passed': sum(row['passed'] for row in checks), 'total': len(checks), 'checks': checks,
              'scope': 'Synthetic API/answer regression screen; no universal model-quality claim.'}
    write_json(args.output / 'summary.json', result)
    print(json.dumps({'passed': result['passed'], 'total': result['total']}), flush=True)
    return 0 if result['passed'] == result['total'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
