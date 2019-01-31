[@deriving (ord, sexp_of)]
type t =
  | Dist(Dist.t)
  | Link(Link.t);

let manifest = (src: t) =>
  switch (src) {
  | Link(info) => info.manifest
  | Dist(dist) => Dist.manifest(dist)
  };

let toDist = (src: t) =>
  switch (src) {
  | Dist(dist) => dist
  | Link({path, manifest, kind: _}) => Dist.LocalPath({path, manifest})
  };

let show' = (~pretty, source) => {
  let showPath = pretty ? DistPath.showPretty : DistPath.show;
  switch (source) {
  | Dist(Github({user, repo, commit, manifest: None})) =>
    Printf.sprintf("github:%s/%s#%s", user, repo, commit)
  | Dist(Github({user, repo, commit, manifest: Some(manifest)})) =>
    Printf.sprintf(
      "github:%s/%s:%s#%s",
      user,
      repo,
      ManifestSpec.show(manifest),
      commit,
    )
  | Dist(Git({remote, commit, manifest: None})) =>
    Printf.sprintf("git:%s#%s", remote, commit)
  | Dist(Git({remote, commit, manifest: Some(manifest)})) =>
    Printf.sprintf(
      "git:%s:%s#%s",
      remote,
      ManifestSpec.show(manifest),
      commit,
    )
  | Dist(Archive({url, checksum})) =>
    Printf.sprintf("archive:%s#%s", url, Checksum.show(checksum))
  | Dist(LocalPath({path, manifest: None})) =>
    Printf.sprintf("path:%s", showPath(path))
  | Dist(LocalPath({path, manifest: Some(manifest)})) =>
    Printf.sprintf(
      "path:%s/%s",
      showPath(path),
      ManifestSpec.show(manifest),
    )
  | Dist(NoSource) => "no-source:"
  | Link(link) => pretty ? Link.showPretty(link) : Link.show(link)
  };
};

let show = show'(~pretty=false);
let showPretty = show'(~pretty=true);

let pp = (fmt, src) => Fmt.pf(fmt, "%s", show(src));

let ppPretty = (fmt, src) => Fmt.pf(fmt, "%s", showPretty(src));

module Parse = {
  include Parse;

  let dist = {
    let%map dist = Dist.parser;
    Dist(dist);
  };

  let link = {
    let%map link = Link.parser;
    Link(link);
  };

  let parser = link <|> dist;

  let parserRelaxed = {
    let distRelaxed = {
      let%map dist = Dist.parserRelaxed;
      Dist(dist);
    };

    parser <|> distRelaxed;
  };
};

let parser = Parse.parser;
let parserRelaxed = Parse.parserRelaxed;
let parse = Parse.(parse(parser));
let parseRelaxed = Parse.(parse(parserRelaxed));

let to_yojson = v => `String(show(v));

let of_yojson = json =>
  switch (json) {
  | `String(string) => parse(string)
  | _ => Error("expected string")
  };

let relaxed_of_yojson = json =>
  switch (json) {
  | `String(string) =>
    let parse = Parse.(parse(parserRelaxed));
    parse(string);
  | _ => Error("expected string")
  };

module Map =
  Map.Make({
    type nonrec t = t;
    let compare = compare;
  });

module Set =
  Set.Make({
    type nonrec t = t;
    let compare = compare;
  });

let%test_module "parsing" =
  (module
   {
     let parse = Parse.Test.parse(~sexp_of=sexp_of_t, parse);

     let%expect_test "github:user/repo#abc123" = {
       parse("github:user/repo#abc123");
       %expect
       {| (Dist (Github (user user) (repo repo) (commit abc123) (manifest ()))) |};
     };

     let%expect_test "github:user/repo:lwt.opam#abc123" = {
       parse("github:user/repo:lwt.opam#abc123");
       %expect
       {|
      (Dist
       (Github (user user) (repo repo) (commit abc123)
        (manifest ((Opam lwt.opam))))) |};
     };

     let%expect_test "gh:user/repo#abc123" = {
       parse("gh:user/repo#abc123");
       %expect
       {| (Dist (Github (user user) (repo repo) (commit abc123) (manifest ()))) |};
     };

     let%expect_test "gh:user/repo:lwt.opam#abc123" = {
       parse("gh:user/repo:lwt.opam#abc123");
       %expect
       {|
      (Dist
       (Github (user user) (repo repo) (commit abc123)
        (manifest ((Opam lwt.opam))))) |};
     };

     let%expect_test "git:http://example.com/repo#abc123" = {
       parse("git:http://example.com/repo#abc123");
       %expect
       {| (Dist (Git (remote http://example.com/repo) (commit abc123) (manifest ()))) |};
     };

     let%expect_test "git:http://example.com/repo:lwt.opam#abc123" = {
       parse("git:http://example.com/repo:lwt.opam#abc123");
       %expect
       {|
      (Dist
       (Git (remote http://example.com/repo) (commit abc123)
        (manifest ((Opam lwt.opam))))) |};
     };

     let%expect_test "git:git://example.com/repo:lwt.opam#abc123" = {
       parse("git:git://example.com/repo:lwt.opam#abc123");
       %expect
       {|
      (Dist
       (Git (remote git://example.com/repo) (commit abc123)
        (manifest ((Opam lwt.opam))))) |};
     };

     let%expect_test "archive:http://example.com#abc123" = {
       parse("archive:http://example.com#abc123");
       %expect
       {| (Dist (Archive (url http://example.com) (checksum (Sha1 abc123)))) |};
     };

     let%expect_test "archive:https://example.com#abc123" = {
       parse("archive:https://example.com#abc123");
       %expect
       {| (Dist (Archive (url https://example.com) (checksum (Sha1 abc123)))) |};
     };

     let%expect_test "archive:https://example.com#md5:abc123" = {
       parse("archive:https://example.com#md5:abc123");
       %expect
       {| (Dist (Archive (url https://example.com) (checksum (Md5 abc123)))) |};
     };

     let%expect_test "path:/some/path" = {
       parse("path:/some/path");
       %expect
       {| (Dist (LocalPath ((path /some/path) (manifest ())))) |};
     };

     let%expect_test "path:/some/path/lwt.opam" = {
       parse("path:/some/path/lwt.opam");
       %expect
       {| (Dist (LocalPath ((path /some/path) (manifest ((Opam lwt.opam)))))) |};
     };

     let%expect_test "link:/some/path" = {
       parse("link:/some/path");
       %expect
       {| (Link (path /some/path) (manifest ()) (kind LinkRegular)) |};
     };

     let%expect_test "link:/some/path/lwt.opam" = {
       parse("link:/some/path/lwt.opam");
       %expect
       {| (Link (path /some/path) (manifest ((Opam lwt.opam))) (kind LinkRegular)) |};
     };

     let%expect_test "path:some" = {
       parse("path:some");
       %expect
       {| (Dist (LocalPath ((path some) (manifest ())))) |};
     };

     let%expect_test "link:some" = {
       parse("link:some");
       %expect
       {| (Link (path some) (manifest ()) (kind LinkRegular)) |};
     };

     let%expect_test "no-source:" = {
       parse("no-source:");
       %expect
       {| (Dist NoSource) |};
     };

     let parseRelaxed = Parse.Test.parse(~sexp_of=sexp_of_t, parseRelaxed);

     let%expect_test "user/repo#abc123" = {
       parseRelaxed("user/repo#abc123");
       %expect
       {| (Dist (Github (user user) (repo repo) (commit abc123) (manifest ()))) |};
     };

     let%expect_test "user/repo:lwt.opam#abc123" = {
       parseRelaxed("user/repo:lwt.opam#abc123");
       %expect
       {|
      (Dist
       (Github (user user) (repo repo) (commit abc123)
        (manifest ((Opam lwt.opam))))) |};
     };

     let%expect_test "http://example.com#abc123" = {
       parseRelaxed("http://example.com#abc123");
       %expect
       {| (Dist (Archive (url http://example.com) (checksum (Sha1 abc123)))) |};
     };

     let%expect_test "https://example.com#abc123" = {
       parseRelaxed("https://example.com#abc123");
       %expect
       {| (Dist (Archive (url https://example.com) (checksum (Sha1 abc123)))) |};
     };

     let%expect_test "https://example.com#md5:abc123" = {
       parseRelaxed("https://example.com#md5:abc123");
       %expect
       {| (Dist (Archive (url https://example.com) (checksum (Md5 abc123)))) |};
     };

     let%expect_test "http://localhost:56886/dep/-/dep-1.0.0.tgz#fabe490fb72a10295d554037341d8c7d5497cde9" = {
       parseRelaxed(
         "http://localhost:56886/dep/-/dep-1.0.0.tgz#fabe490fb72a10295d554037341d8c7d5497cde9",
       );
       %expect
       {|
      (Dist
       (Archive (url http://localhost:56886/dep/-/dep-1.0.0.tgz)
        (checksum (Sha1 fabe490fb72a10295d554037341d8c7d5497cde9)))) |};
     };

     let%expect_test "/some/path" = {
       parseRelaxed("/some/path");
       %expect
       {| (Dist (LocalPath ((path /some/path) (manifest ())))) |};
     };

     let%expect_test "some" = {
       parseRelaxed("some");
       %expect
       {| ERROR: parsing "some": : not a path |};
     };
   });
