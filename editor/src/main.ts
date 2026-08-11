// SPDX-License-Identifier: GPL-3.0-or-later

import { mount } from "svelte";
import App from "./App.svelte";
import "./styles.css";
import "./radial.css";
import "./artwork.css";
import "./context-menu.css";
import "./custom-select.css";

mount(App, { target: document.getElementById("app")! });
