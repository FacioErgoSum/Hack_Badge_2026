"""Template App file to copy and make other apps"""

from apps.base_app import BaseApp
from lib.board import reg_in_v_sns
from lib.button import MenuMinus, MenuPlus, UserA, UserB, Boot
from lib.epd import display
from lib.led_bar import NUM_LEDS, leds

def await_pressed_buttons():
    if MenuMinus.pressed():
        print("Menu -")
    if MenuPlus.pressed():
        print("Menu +")
    if UserA.pressed():
        print("User A")
    if UserB.pressed():
        print("User B")
    if Boot.pressed():
        print("Boot")

class TemplateApp(BaseApp):
    def __init__(self, name: str | None = None) -> None:
        self.name = "Template App"
        self.sleep_ms = 100
        """When your app is running, it will sleep this many ms between calls to run()"""
        self.launched = False
        """This will be True for the first time each app is selected,
        so you can do something each time it is started, like draw something to the screen
        It will be reset to False after the first cycle."""


    def run(self):
        """Add your app behavior here. Will be run in a while True loop."""
        if self.launched:    ## Setup
            pass
        else:                ## Mainloop
            await_pressed_buttons()

