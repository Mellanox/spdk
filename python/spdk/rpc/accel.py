from spdk.rpc.helpers import deprecated_alias


def accel_get_opc_assignments(client):
    """Get list of opcode name to module assignments.
    """
    return client.call('accel_get_opc_assignments')


@deprecated_alias('accel_get_engine_info')
def accel_get_module_info(client):
    """Get list of valid module names and their operations.
    """
    return client.call('accel_get_module_info')


def accel_assign_opc(client, opname, module):
    """Manually assign an operation to a module.

    Args:
        opname: name of operation
        module: name of module
    """
    params = {
        'opname': opname,
        'module': module,
    }

    return client.call('accel_assign_opc', params)


def accel_crypto_key_create(client, module, cipher, key, key2, name):
    """Create Data Encryption Key Identifier.

    Args:
        module: accel module name
        cipher: cipher
        key: key
        key2: key2
        name: key name
    """
    params = {
        'cipher': cipher,
        'key': key,
        'name': name,
    }
    if key2 is not None:
        params['key2'] = key2
    if module is not None:
        params['module'] = module

    return client.call('accel_crypto_key_create', params)


def accel_crypto_keys_get(client, key_name):
    """Get a list of the crypto keys.

    Args:
        key_name: Get information about a specific key
    """
    params = {}

    if key_name is not None:
        params['key_name'] = key_name

    return client.call('accel_crypto_keys_get', params)
