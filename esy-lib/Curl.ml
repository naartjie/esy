module String = Astring.String

type response =
  | Success of string
  | NotFound

type headers = string StringMap.t

type url = string

let parseStdout stdout =
  let open Run.Syntax in
  match String.cut ~rev:true ~sep:"\n" stdout with
  | Some (stdout, httpcode) ->
    let%bind httpcode =
      try return (int_of_string httpcode)
      with Failure _ -> errorf "unable to parse HTTP code: %s" httpcode
    in
    return (stdout, httpcode)
  | None ->
    error "unable to parse metadata from a curl response"

let runCurl cmd =
  let cmd = Cmd.(
    cmd
    % "--write-out"
    % {|\n%{http_code}|}
  ) in
  let f p =
    let%lwt stdout =
      Lwt.finalize
        (fun () -> Lwt_io.read p#stdout)
        (fun () -> Lwt_io.close p#stdout)
    and stderr = Lwt_io.read p#stderr in
    match%lwt p#status with
    | Unix.WEXITED 0 -> begin
      match parseStdout stdout with
      | Ok (stdout, _httpcode) -> RunAsync.return (Success stdout)
      | Error err -> Lwt.return (Error err)
      end
    | _ -> begin
      match parseStdout stdout with
      | Ok (_stdout, httpcode) when httpcode = 404 ->
        RunAsync.return NotFound
      | Ok (_stdout, httpcode) ->
        RunAsync.errorf
          "@[<v>error running curl: %a:@\ncode: %i@\nstderr:@[<v 2>@\n%a@]@]"
          Cmd.pp cmd httpcode Fmt.lines stderr
      | _ ->
        RunAsync.errorf
          "@[<v>error running curl: %a:@\nstderr:@[<v 2>@\n%a@]@]"
          Cmd.pp cmd Fmt.lines stderr
    end
  in
  try%lwt
    EsyBashLwt.with_process_full cmd f
  with
  | Unix.Unix_error (err, _, _) ->
    let msg = Unix.error_message err in
    RunAsync.error msg
  | exn ->
    RunAsync.errorf "error running subprocess %s" (Printexc.exn_slot_name exn)

let getOrNotFound ?accept url =
  let cmd = Cmd.(
    v "curl"
    % "--silent"
    % "--connect-timeout"
    % "60"
    % "--fail"
    % "--location" % url
  ) in
  let cmd =
    match accept with
    | Some accept -> Cmd.(cmd % "--header" % accept)
    | None -> cmd
  in
  runCurl cmd

let head url =
  let open RunAsync.Syntax in

  let parseResponse response =
    match StringLabels.split_on_char ~sep:'\n' response with
    | [] -> StringMap.empty
    | _::lines ->
      let f headers line =
        match String.cut ~sep:":" line with
        | None -> headers
        | Some (name, value) ->
          let name = name |> String.trim |> String.Ascii.lowercase in
          let value = String.trim value in
          StringMap.add name value headers
      in
      List.fold_left ~f ~init:StringMap.empty lines
  in

  let cmd = Cmd.(
    v "curl"
    % "--head"
    % "--silent"
    % "--connect-timeout"
    % "60"
    % "--fail"
    % "--location" % url
  ) in
  match%bind runCurl cmd with
  | Success response -> return (parseResponse response)
  | NotFound -> RunAsync.error "not found"

let get ?accept url =
  let open RunAsync.Syntax in
  match%bind getOrNotFound ?accept url with
  | Success result -> RunAsync.return result
  | NotFound -> RunAsync.error "not found"

let download ~output  url =
  let open RunAsync.Syntax in
  let output = EsyBash.normalizePathForCygwin (Path.show output) in
  let cmd = Cmd.(
    v "curl"
    % "--silent"
    % "--connect-timeout"
    % "60"
    % "--fail"
    % "--location" % url
    % "--output" % output
  ) in
  match%bind runCurl cmd with
  | Success _ -> RunAsync.return ()
  | NotFound -> RunAsync.error "not found"
