import { mount } from "svelte";
import App from "./App.svelte";
import "./styles.css";
import "./m6.css";
import "./artwork.css";
import "./context-menu.css";

mount(App, { target: document.getElementById("app")! });
