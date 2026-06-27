from playwright.sync_api import Page

from .helpers import select_species_below_viridiplantae


def test_uncollapse_to_viridiplantae_and_select_species(rendered_page: Page) -> None:
    select_species_below_viridiplantae(rendered_page)
