from playwright.sync_api import Page, Request, expect

from .helpers import (
    STEP_TIMEOUT_MS,
    click_tree_node,
    expand_tree_node,
    log_step,
    wait_for_selected_count,
    wait_for_tree_label,
)


def _zoom_percent(page: Page) -> int:
    value = page.locator("#treeZoomValue").inner_text().strip()
    return int((value or "0").rstrip("%"))


def test_tree_wheel_zoom_pan_and_reset(rendered_page: Page) -> None:
    log_step("Checking that tree viewport controls are enabled after render")
    expect(rendered_page.locator("#treeZoomOut")).to_be_enabled(
        timeout=STEP_TIMEOUT_MS,
    )
    expect(rendered_page.locator("#treeZoomIn")).to_be_enabled(
        timeout=STEP_TIMEOUT_MS,
    )
    expect(rendered_page.locator("#treeZoomValue")).to_have_text("100%")

    chart = rendered_page.locator("#chartWrap")
    svg_before = rendered_page.locator("#treeSvg").evaluate(
        """
        (svg) => ({
          viewBox: svg.getAttribute('viewBox'),
          width: svg.getBoundingClientRect().width,
          height: svg.getBoundingClientRect().height,
        })
        """
    )
    chart_box = chart.bounding_box()
    assert chart_box is not None
    pointer_x = chart_box["x"] + (chart_box["width"] * 0.7)
    pointer_y = chart_box["y"] + (chart_box["height"] * 0.7)

    tree_requests: list[str] = []

    def record_tree_request(request: Request) -> None:
        if any(
            endpoint in request.url
            for endpoint in ("/root-view", "/expand-node", "/uncollapse-to-tips")
        ):
            tree_requests.append(request.url)

    rendered_page.on("request", record_tree_request)
    try:
        log_step("Zooming in with the mouse wheel")
        rendered_page.mouse.move(pointer_x, pointer_y)
        rendered_page.mouse.wheel(0, -900)
        rendered_page.wait_for_function(
            """
            () => Number(
              (document.getElementById('treeZoomValue')?.textContent || '0')
                .replace('%', '')
            ) > 100
            """,
            timeout=STEP_TIMEOUT_MS,
        )
        zoomed_percent = _zoom_percent(rendered_page)
        assert zoomed_percent > 100

        svg_after_zoom = rendered_page.locator("#treeSvg").evaluate(
            """
            (svg) => ({
              viewBox: svg.getAttribute('viewBox'),
              width: svg.getBoundingClientRect().width,
              height: svg.getBoundingClientRect().height,
            })
            """
        )
        assert svg_after_zoom["viewBox"] == svg_before["viewBox"]
        assert svg_after_zoom["width"] > svg_before["width"]
        assert svg_after_zoom["height"] > svg_before["height"]

        log_step("Zooming out with the mouse wheel")
        rendered_page.mouse.wheel(0, 400)
        rendered_page.wait_for_function(
            """
            (previousZoom) => Number(
              (document.getElementById('treeZoomValue')?.textContent || '0')
                .replace('%', '')
            ) < previousZoom
            """,
            arg=zoomed_percent,
            timeout=STEP_TIMEOUT_MS,
        )
        assert not tree_requests
    finally:
        rendered_page.remove_listener("request", record_tree_request)

    log_step("Checking drag panning after zoom")
    pan_before = rendered_page.evaluate(
        """
        () => {
          const wrap = document.getElementById('chartWrap');
          wrap.scrollLeft = 0;
          wrap.scrollTop = 0;
          return { left: wrap.scrollLeft, top: wrap.scrollTop };
        }
        """
    )
    rendered_page.mouse.move(pointer_x, pointer_y)
    rendered_page.mouse.down()
    rendered_page.mouse.move(pointer_x - 120, pointer_y - 80, steps=4)
    rendered_page.mouse.up()
    rendered_page.wait_for_function(
        """
        (before) => {
          const wrap = document.getElementById('chartWrap');
          return wrap.scrollLeft > before.left || wrap.scrollTop > before.top;
        }
        """,
        arg=pan_before,
        timeout=STEP_TIMEOUT_MS,
    )

    # Drag panning intentionally suppresses the immediately following click.
    rendered_page.wait_for_timeout(150)

    log_step("Checking selection and expansion after zoom")
    wait_for_tree_label(rendered_page, "cellular organisms")
    click_tree_node(rendered_page, "cellular organisms", modifiers=["Meta"])
    wait_for_selected_count(rendered_page, "1")
    expand_tree_node(rendered_page, "cellular organisms", 131567)

    log_step("Resetting tree zoom")
    rendered_page.locator("#treeZoomReset").click()
    expect(rendered_page.locator("#treeZoomValue")).to_have_text(
        "100%",
        timeout=STEP_TIMEOUT_MS,
    )
