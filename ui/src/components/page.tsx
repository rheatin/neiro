// A page of the main stack: its content on the app's own background. NSH's
// navigation stack paints pages its own grey (the Linux libadwaita fill),
// so the page paints over it; on the desktops the header's controls live
// in the title bar, so a page is just its painted content.
//
// A page that remembers its scroll (`rememberScroll`) also floats the
// 回到顶部 button here, over its content and above the player bar.

import { type JSX, View } from "nsh";
import { NavPage } from "nsh/base";
import { pageAnimation } from "../nav";
import type { PageScroll } from "../state/page-scroll";
import { selectionActive } from "../state/queue";
import { BackToTop } from "./ui/back-to-top";

export function AppPage(props: { title: string; testID: string; children: JSX.Element; scroll?: PageScroll }) {
  return (
    <NavPage title={props.title} testID={props.testID} animation={pageAnimation()}>
      <View class="absolute inset-0 bg-sidebar">
        {props.children}
        {/* 多选时批量条占着同一个角落，回到顶部让开。 */}
        <If when={(props.scroll?.showTop() ?? false) && !selectionActive()}>
          <BackToTop onPress={() => props.scroll?.toTop()} />
        </If>
      </View>
    </NavPage>
  );
}
