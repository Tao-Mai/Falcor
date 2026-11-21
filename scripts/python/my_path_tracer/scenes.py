# Copyright (c) 2026, Falcor project contributors.
# SPDX-License-Identifier: BSD-3-Clause
"""实验场景清单与 Falcor 原生场景导入；资产保留在代码仓库外。"""

import json
import os
from pathlib import Path
import re
import xml.etree.ElementTree as ET

REPOSITORY = Path(__file__).resolve().parents[3]
CATALOG_PATH = Path(__file__).with_name("scene_catalog.json")


def load_catalog():
    return json.loads(CATALOG_PATH.read_text(encoding="utf-8"))


def default_assets_root():
    return Path(os.environ.get("FALCOR_BENCHMARK_ASSETS", REPOSITORY.parent / "FalcorResearchAssets"))


def select_scenes(catalog, suite, identifiers=None):
    selected = identifiers or catalog["suites"][suite]
    indexed = {scene["id"]: scene for scene in catalog["scenes"]}
    unknown = set(selected) - indexed.keys()
    if unknown:
        raise ValueError("未知场景: " + ", ".join(sorted(unknown)))
    return [indexed[name] for name in dict.fromkeys(selected)]


def resolve_scene(scene, assets_root):
    if scene.get("unavailable"):
        return None, scene["unavailable"]
    root = REPOSITORY if scene["root"] == "repository" else Path(assets_root).resolve()
    relative = Path(scene["path"])
    if relative.is_absolute() or ".." in relative.parts:
        raise ValueError("场景路径必须是资产目录内的相对路径")
    # Falcor's media directory is normally a Packman junction outside the checkout.
    path = (root / relative).resolve()
    if not path.is_file():
        return None, "资产未安装: " + str(path)
    return path, None


def scene_import_script(scene, path, double_sided_ids=()):
    """生成可审阅的 pyscene，使用当前 Falcor 的 importer 和脚本上下文。"""
    lines = ["sceneBuilder.importScene(" + repr(str(path)) + ")"]
    multiplier = scene.get("emission_multiplier", 1.0)
    if multiplier != 1.0:
        lines.extend([
            "for material in sceneBuilder.materials:",
            "    if hasattr(material, 'emissiveFactor'):",
            "        material.emissiveFactor *= " + repr(float(multiplier)),
        ])
    if double_sided_ids:
        lines.extend([
            "for material in sceneBuilder.materials:",
            "    if material.name in " + repr(list(double_sided_ids)) + ":",
            "        material.doubleSided = True",
        ])
    return "\n".join(lines) + "\n"


def prepare_mitsuba_scene(path, output):
    """保留资产文件，生成当前 Falcor importer 可读取的兼容 XML。

    当前 importer 尚未实现 <default>，其 matrix 读取约定也与资产的
    Mitsuba 行排列不同。展开默认参数、固定资源路径、适配矩阵输入，
    并通过场景脚本恢复双面标志，保留原始资产文件。
    """
    tree = ET.parse(path)
    root = tree.getroot()
    defaults = {node.attrib["name"]: node.attrib["value"] for node in root.findall("default")}
    for node in list(root.findall("default")):
        root.remove(node)
    # The current native twosided branch dereferences its unassigned outer
    # material. Import the inner BSDF and restore its native doubleSided flag
    # through the standard scene script, without changing Falcor source.
    double_sided_ids = []
    for parent in list(root.iter())[::-1]:
        for wrapper in list(parent):
            if wrapper.tag != "bsdf" or wrapper.get("type") != "twosided":
                continue
            children = wrapper.findall("bsdf")
            if len(children) != 1:
                raise ValueError("兼容场景仅支持包含单个 BSDF 的 twosided")
            inner = children[0]
            identifier = wrapper.get("id", "benchmark_twosided_" + str(len(double_sided_ids)))
            inner.set("id", identifier)
            if wrapper.get("name"):
                inner.set("name", wrapper.get("name"))
            index = list(parent).index(wrapper)
            parent.remove(wrapper)
            parent.insert(index, inner)
            double_sided_ids.append(identifier)
    for node in root.iter():
        for name, value in list(node.attrib.items()):
            node.attrib[name] = re.sub(r"\$([A-Za-z_][A-Za-z0-9_]*)", lambda match: defaults[match.group(1)], value)
        if node.tag == "string" and node.get("name") == "filename":
            resource = (path.parent / node.attrib["value"]).resolve()
            if not resource.is_file():
                raise FileNotFoundError(resource)
            node.set("value", resource.as_posix())
        if node.tag == "matrix":
            tokens = node.attrib["value"].split()
            dimension = {9: 3, 16: 4}.get(len(tokens))
            if dimension is None:
                raise ValueError("不支持的 Mitsuba 矩阵大小")
            node.set("value", " ".join(tokens[column * dimension + row]
                                       for row in range(dimension) for column in range(dimension)))
    destination = output / "scene_prepared.xml"
    tree.write(destination, encoding="utf-8", xml_declaration=True)
    (output / "mitsuba_compatibility.json").write_text(json.dumps({
        "source": str(path), "resolved_defaults": defaults, "double_sided_ids": double_sided_ids,
        "matrix_input_transposed_for_native_importer": True,
        "original_asset_modified": False,
    }, indent=2) + "\n", encoding="utf-8")
    return destination
