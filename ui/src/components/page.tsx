// A page of the main stack: its content on the app's own background. NSH's
// navigation stack paints pages its own grey (the Linux libadwaita fill),
// so the page paints over it; on the desktops the header's controls live
// in the title bar, so a page is just its painted content.

import { type JSX, View } from "nsh";
import { NavPage } from "nsh/base";
import { pageAnimation } from "../nav";

export function AppPage(props: { title: string; testID: string; children: JSX.Element }) {
  return (
    <NavPage title={props.title} testID={props.testID} animation={pageAnimation()}>
      <View class="absolute inset-0 bg-sidebar">{props.children}</View>
    </NavPage>
  );
}
