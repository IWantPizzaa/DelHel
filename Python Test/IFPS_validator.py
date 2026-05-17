#!/usr/bin/env python3
"""
Automatic IFPS validator using EUROCONTROL NM B2B validateFlightPlan.

Input flight plan format:
- Full ICAO 2012 FPL message text (e.g. starts with "(FPL-" and ends with ")").
- Multi-line and single-line messages are both accepted.

Validation verdict:
- GOOD: IFPS status is VALIDATED or READY_TO_SEND and no errors are returned.
- NOT GOOD: IFPS status is INVALID, IFPS returns one or more errors, or IFPS
  still reports PROCESSING.

Usage examples: 
python "Python Test/IFPS_validator.py" --flight-plan-file plan.txt --cert client.pem --key client.key --json
"""

from __future__ import annotations

import argparse
import json
import sys
from dataclasses import dataclass, asdict
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional, Tuple


DEFAULT_WSDL_URL = (
    "https://www.b2b.nm.eurocontrol.int/B2B_PREOPS/gateway/spec/flightPreparationService.wsdl"
)
DEFAULT_ENDPOINT_URL = (
    "https://www.b2b.nm.eurocontrol.int/B2B_PREOPS/gateway/spec/flightPreparationService"
)


@dataclass
class ValidationOutcome:
    verdict: str
    is_good: bool
    status: Optional[str]
    errors: List[Dict[str, Any]]
    warnings: List[str]
    notes: List[str]
    raw_response: Dict[str, Any]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Validate an ICAO flight plan against IFPS (NM B2B validateFlightPlan)."
    )
    source = parser.add_mutually_exclusive_group(required=False)
    source.add_argument(
        "--flight-plan",
        help="Full ICAO 2012 FPL message text (prefer quoting or @file).",
    )
    source.add_argument(
        "--flight-plan-file",
        help="Path to a text file that contains the full ICAO 2012 FPL message.",
    )

    parser.add_argument(
        "--wsdl-url",
        default=DEFAULT_WSDL_URL,
        help=f"WSDL URL (default: {DEFAULT_WSDL_URL})",
    )
    parser.add_argument(
        "--endpoint-url",
        default=DEFAULT_ENDPOINT_URL,
        help=f"SOAP endpoint URL (default: {DEFAULT_ENDPOINT_URL})",
    )
    parser.add_argument(
        "--cert",
        help="Path to client certificate (.pem, .p12 depending on requests/OpenSSL setup).",
    )
    parser.add_argument(
        "--key",
        help="Path to private key file if certificate and key are separate files.",
    )
    parser.add_argument(
        "--ca-bundle",
        help="Path to CA bundle file for TLS server certificate validation.",
    )
    parser.add_argument(
        "--insecure",
        action="store_true",
        help="Disable TLS verification (only for local/testing troubleshooting).",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=30.0,
        help="HTTP/SOAP timeout in seconds (default: 30).",
    )
    parser.add_argument(
        "--json",
        action="store_true",
        help="Print machine-readable JSON output.",
    )
    parser.add_argument(
        "--debug",
        action="store_true",
        help="Print extra diagnostics (operation signature and raw response keys).",
    )
    return parser.parse_args()


def read_flight_plan(args: argparse.Namespace) -> str:
    if args.flight_plan:
        plan = args.flight_plan
    elif args.flight_plan_file:
        plan = Path(args.flight_plan_file).read_text(encoding="utf-8")
    elif not sys.stdin.isatty():
        plan = sys.stdin.read()
    else:
        raise ValueError(
            "No flight plan provided. Use --flight-plan, --flight-plan-file, or pipe via stdin."
        )

    plan = normalize_flight_plan(plan)
    validate_minimum_icao_shape(plan)
    return plan


def normalize_flight_plan(plan: str) -> str:
    cleaned = plan.replace("\r\n", "\n").strip()
    return cleaned


def validate_minimum_icao_shape(plan: str) -> None:
    uppercase = plan.upper()
    if "(FPL-" not in uppercase:
        raise ValueError(
            "Expected full ICAO FPL message. It should include '(FPL-' and all ICAO fields."
        )
    if not uppercase.endswith(")"):
        raise ValueError("Expected full ICAO FPL message to end with ')' character.")
    if uppercase.count("-") < 5:
        raise ValueError(
            "FPL looks incomplete. Provide full ICAO 2012 FPL text, not only route fragments."
        )


def import_dependencies() -> Tuple[Any, Any, Any, Any, Any, Any, Any]:
    try:
        import requests
        from zeep import Client, Settings
        from zeep.exceptions import Fault, ValidationError
        from zeep.helpers import serialize_object
        from zeep.transports import Transport
    except ImportError as exc:
        raise RuntimeError(
            "Missing dependency. Install with: pip install requests zeep"
        ) from exc

    return requests, Client, Settings, Transport, serialize_object, Fault, ValidationError


def build_soap_service(
    *,
    requests: Any,
    Client: Any,
    Settings: Any,
    Transport: Any,
    wsdl_url: str,
    endpoint_url: str,
    cert: Optional[str],
    key: Optional[str],
    ca_bundle: Optional[str],
    insecure: bool,
    timeout: float,
) -> Tuple[Any, Any, Any]:
    session = requests.Session()
    if insecure:
        session.verify = False
    elif ca_bundle:
        session.verify = ca_bundle
    else:
        session.verify = True

    if cert and key:
        session.cert = (cert, key)
    elif cert:
        session.cert = cert

    transport = Transport(session=session, timeout=timeout, operation_timeout=timeout)
    settings = Settings(strict=False, xml_huge_tree=True)
    client = Client(wsdl=wsdl_url, transport=transport, settings=settings)

    binding_qname = find_binding_for_operation(client, "validateFlightPlan")
    if binding_qname is None:
        raise RuntimeError(
            "Could not find validateFlightPlan operation in WSDL. Check --wsdl-url."
        )

    service = client.create_service(binding_qname, endpoint_url)
    operation = client.wsdl.bindings[binding_qname]._operations["validateFlightPlan"]
    return client, service, operation


def find_binding_for_operation(client: Any, operation_name: str) -> Optional[str]:
    for binding_qname, binding in client.wsdl.bindings.items():
        if operation_name in binding._operations:
            return str(binding_qname)
    return None


def list_operation_parameters(operation: Any) -> List[str]:
    names: List[str] = []
    body = getattr(operation.input, "body", None)
    if not body:
        return names
    body_type = getattr(body, "type", None)
    if not body_type:
        return names
    elements = getattr(body_type, "elements", None) or []
    for element in elements:
        try:
            names.append(element[0])
        except Exception:
            continue
    return names


def call_validate_flight_plan(
    *,
    service: Any,
    operation: Any,
    flight_plan: str,
) -> Tuple[Any, str]:
    param_names = list_operation_parameters(operation)
    attempts: List[Tuple[str, Any]] = []

    if param_names:
        kwargs: Dict[str, Any] = {}
        inserted_plan = False
        for name in param_names:
            lname = name.lower()
            if "flightplan" in lname or lname == "flight_plan":
                kwargs[name] = flight_plan
                inserted_plan = True
            elif "time" in lname:
                kwargs[name] = datetime.now(timezone.utc)
        if not inserted_plan and len(param_names) == 1:
            kwargs[param_names[0]] = flight_plan
            inserted_plan = True
        if inserted_plan:
            attempts.append(("keyword call from WSDL parameters", lambda: service.validateFlightPlan(**kwargs)))

    attempts.extend(
        [
            ("keyword flightPlan", lambda: service.validateFlightPlan(flightPlan=flight_plan)),
            ("positional string", lambda: service.validateFlightPlan(flight_plan)),
            ("wrapped dict flightPlan", lambda: service.validateFlightPlan({"flightPlan": flight_plan})),
        ]
    )

    errors: List[str] = []
    for label, fn in attempts:
        try:
            return fn(), label
        except Exception as exc:  # noqa: BLE001 - keep robust against SOAP client variants
            errors.append(f"{label}: {exc}")
            continue

    combined = "\n".join(errors)
    raise RuntimeError(f"Unable to call validateFlightPlan with detected signatures.\n{combined}")


def as_dict(serialize_object: Any, response: Any) -> Dict[str, Any]:
    raw = serialize_object(response, target_cls=dict)
    if isinstance(raw, dict):
        return raw
    return {"value": raw}


def _walk_nodes(obj: Any) -> Iterable[Any]:
    stack = [obj]
    while stack:
        current = stack.pop()
        yield current
        if isinstance(current, dict):
            stack.extend(current.values())
        elif isinstance(current, list):
            stack.extend(current)


def extract_status(data: Dict[str, Any]) -> Optional[str]:
    status_keys = {"status", "flightPlanStatus", "validationStatus"}
    for node in _walk_nodes(data):
        if isinstance(node, dict):
            for key, value in node.items():
                if key in status_keys and isinstance(value, str):
                    return value
    return None


def extract_errors(data: Dict[str, Any]) -> List[Dict[str, Any]]:
    out: List[Dict[str, Any]] = []
    for node in _walk_nodes(data):
        if isinstance(node, dict):
            for key, value in node.items():
                if key == "errors" and isinstance(value, list):
                    for entry in value:
                        if isinstance(entry, dict):
                            out.append(entry)
                        else:
                            out.append({"message": str(entry)})
                elif key == "error" and isinstance(value, dict):
                    out.append(value)
    return out


def extract_warnings(data: Dict[str, Any]) -> List[str]:
    warnings: List[str] = []
    for node in _walk_nodes(data):
        if isinstance(node, dict):
            for key, value in node.items():
                if key.lower().startswith("warning"):
                    if isinstance(value, str):
                        warnings.append(value)
                    elif isinstance(value, list):
                        warnings.extend(str(v) for v in value)
    return warnings


def evaluate_outcome(raw_response: Dict[str, Any]) -> ValidationOutcome:
    status = extract_status(raw_response)
    errors = extract_errors(raw_response)
    warnings = extract_warnings(raw_response)
    notes: List[str] = []

    normalized = (status or "").upper()
    good_status = {"VALIDATED", "READY_TO_SEND"}
    bad_status = {"INVALID"}
    pending_status = {"PROCESSING"}

    if normalized in good_status and not errors:
        verdict = "GOOD"
        is_good = True
    elif normalized in bad_status or errors:
        verdict = "NOT GOOD"
        is_good = False
    elif normalized in pending_status:
        verdict = "NOT GOOD"
        is_good = False
        notes.append("IFPS returned PROCESSING; no final acceptance result yet.")
    else:
        verdict = "NOT GOOD"
        is_good = False
        notes.append("No definitive GOOD status detected in IFPS reply.")

    if not status:
        notes.append("No status field found in IFPS response payload.")

    return ValidationOutcome(
        verdict=verdict,
        is_good=is_good,
        status=status,
        errors=errors,
        warnings=warnings,
        notes=notes,
        raw_response=raw_response,
    )


def print_human_readable(outcome: ValidationOutcome, debug: bool = False, call_mode: str = "") -> None:
    print(f"Verdict: {outcome.verdict}")
    print(f"Status: {outcome.status or 'UNKNOWN'}")
    if call_mode:
        print(f"Call mode: {call_mode}")

    if outcome.errors:
        print(f"Errors: {len(outcome.errors)}")
        for idx, err in enumerate(outcome.errors, 1):
            reason = err.get("reason") or err.get("message") or "Unknown error"
            code = err.get("code")
            associated = err.get("associatedData")
            invalid_data = err.get("invalidData")
            parts = [f"{idx}. {reason}"]
            if code is not None:
                parts.append(f"code={code}")
            if associated:
                parts.append(f"associatedData={associated}")
            if invalid_data:
                parts.append(f"invalidData={invalid_data}")
            print(" | ".join(parts))
    else:
        print("Errors: 0")

    if outcome.warnings:
        print(f"Warnings: {len(outcome.warnings)}")
        for idx, warning in enumerate(outcome.warnings, 1):
            print(f"{idx}. {warning}")
    else:
        print("Warnings: 0")

    for note in outcome.notes:
        print(f"Note: {note}")

    print("GOOD rule: status in {VALIDATED, READY_TO_SEND} and no errors.")
    print("NOT GOOD rule: status INVALID, PROCESSING, unknown status, or any error present.")

    if debug:
        print(f"Raw response keys: {sorted(outcome.raw_response.keys())}")


def main() -> int:
    args = parse_args()

    try:
        flight_plan = read_flight_plan(args)
    except Exception as exc:  # noqa: BLE001
        print(f"Input error: {exc}", file=sys.stderr)
        return 2

    try:
        (
            requests,
            Client,
            Settings,
            Transport,
            serialize_object,
            Fault,
            ValidationError,
        ) = import_dependencies()
    except Exception as exc:  # noqa: BLE001
        print(f"Validation setup failed: {exc}", file=sys.stderr)
        return 3

    try:
        client, service, operation = build_soap_service(
            requests=requests,
            Client=Client,
            Settings=Settings,
            Transport=Transport,
            wsdl_url=args.wsdl_url,
            endpoint_url=args.endpoint_url,
            cert=args.cert,
            key=args.key,
            ca_bundle=args.ca_bundle,
            insecure=args.insecure,
            timeout=args.timeout,
        )

        if args.debug:
            params = list_operation_parameters(operation)
            print(
                f"Detected validateFlightPlan parameters: {params if params else '[unknown/none]'}",
                file=sys.stderr,
            )

        response, call_mode = call_validate_flight_plan(
            service=service,
            operation=operation,
            flight_plan=flight_plan,
        )
        raw = as_dict(serialize_object, response)
        outcome = evaluate_outcome(raw)
    except (Fault, ValidationError) as exc:
        print(f"IFPS SOAP error: {exc}", file=sys.stderr)
        return 3
    except Exception as exc:  # noqa: BLE001
        print(f"Validation request failed: {exc}", file=sys.stderr)
        return 3

    if args.json:
        payload = asdict(outcome)
        payload["call_mode"] = call_mode
        print(json.dumps(payload, indent=2, default=str))
    else:
        print_human_readable(outcome, debug=args.debug, call_mode=call_mode)

    return 0 if outcome.is_good else 1


if __name__ == "__main__":
    raise SystemExit(main())
