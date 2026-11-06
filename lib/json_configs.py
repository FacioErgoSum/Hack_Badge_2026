import json

DEFAULT_CONFIGFILE = "data/panel_config.json"

def load(filename=DEFAULT_CONFIGFILE):
    """configs = configs.load()
       configs["board_type"] 
       """
    return json.load(open(filename))

def store(dictionary, filename=DEFAULT_CONFIGFILE):
    """just jsonifies and writes the dictionary"""
    with open(filename, "w") as f:
        json.dump(dictionary, f)

def append(entry, entry_name, filename=DEFAULT_CONFIGFILE):
    """a = "some data"
       configs.append(a, "my_data_name")
       c = configs.load()
       c["my_data_name"] -> "some data"
       """
    json_dict = load(filename)
    json_dict[entry_name] = entry
    store(json_dict, filename)


