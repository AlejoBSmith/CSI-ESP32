"""Persist receiver aliases by device identity, independently of COM numbers."""
import json
from pathlib import Path


ALIASES = ('rx1', 'rx2', 'rx3')


class RegistryError(ValueError):
    pass


def normalized_identity(value):
    if not isinstance(value, str) or not value.strip() or any(ord(c) < 32 for c in value):
        raise RegistryError('Receiver identity must be a nonempty string without control characters.')
    return value.strip().casefold()


def validate_nodes(nodes):
    if not isinstance(nodes, dict):
        raise RegistryError('The node registry must contain an object named nodes.')
    if len(nodes) > len(ALIASES):
        raise RegistryError('The node registry supports at most 3 receivers.')
    result = {}
    seen = set()
    for alias, identity in nodes.items():
        if alias not in ALIASES:
            raise RegistryError('Receiver aliases must be rx1, rx2 or rx3.')
        identity = normalized_identity(identity)
        if identity in seen:
            raise RegistryError('Duplicate receiver identity in the node registry.')
        seen.add(identity)
        result[alias] = identity
    return result


def _unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise RegistryError(f'Duplicate JSON key in node registry: {key}.')
        result[key] = value
    return result


def load_registry(path):
    path = Path(path)
    try:
        document = json.loads(path.read_text(encoding='utf-8'), object_pairs_hook=_unique_object)
    except FileNotFoundError:
        return {}
    except (OSError, ValueError) as error:
        raise RegistryError(f'Cannot read node registry {path}: {error}') from error
    if not isinstance(document, dict) or set(document) != {'version', 'nodes'}:
        raise RegistryError(f'Invalid node registry {path}: expected version and nodes.')
    if type(document['version']) is not int or document['version'] != 1:
        raise RegistryError(f'Unsupported node registry version in {path}.')
    return validate_nodes(document['nodes'])


def assign_receivers(receivers, nodes):
    """Reserve absent aliases; add new identities in the original sorted order.

    Input records have the launcher's existing (role, identity, port) shape.
    Return (alias, identity, port) records and the complete updated registry.
    """
    updated = validate_nodes(nodes)
    discovered = []
    seen = set()
    for role, identity, port in receivers:
        original_identity = identity
        identity = normalized_identity(identity)
        if role != 'RX' or not isinstance(port, str) or not port:
            raise RegistryError('Invalid discovered receiver record.')
        if identity in seen:
            raise RegistryError(f'Duplicate discovered receiver identity: {identity}.')
        seen.add(identity)
        discovered.append((identity, port, original_identity))
    if len(discovered) > len(ALIASES):
        raise RegistryError('This interface supports at most 3 receivers.')
    by_identity = {identity: alias for alias, identity in updated.items()}
    free = [alias for alias in ALIASES if alias not in updated]
    # Keep the previous launcher's case-sensitive sort on a first launch.
    ordered = sorted(discovered, key=lambda item: (item[2], item[1]))
    unknown = [identity for identity, _, _ in ordered if identity not in by_identity]
    if len(unknown) > len(free):
        raise RegistryError('All 3 receiver aliases are reserved; remove a retired identity from the node registry before adding another receiver.')
    for alias, identity in zip(free, unknown):
        updated[alias] = identity
        by_identity[identity] = alias
    assignments = sorted((by_identity[identity], identity, port) for identity, port, _ in discovered)
    return assignments, updated


def save_registry(path, nodes):
    path = Path(path)
    document = dict(version=1, nodes=validate_nodes(nodes))
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        temporary = path.with_name(path.name + '.tmp')
        temporary.write_text(json.dumps(document, indent=2, sort_keys=True) + '\n', encoding='utf-8')
        temporary.replace(path)
    except OSError as error:
        raise RegistryError(f'Cannot save node registry {path}: {error}') from error
