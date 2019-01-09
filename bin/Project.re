open EsyPackageConfig;
open EsyInstall;
open EsyBuild;

type project = {
  projcfg: ProjectConfig.t,
  spec: SandboxSpec.t,
  workflow: Workflow.t,
  buildCfg: EsyBuildPackage.Config.t,
  solveSandbox: EsySolve.Sandbox.t,
  installSandbox: EsyInstall.Sandbox.t,
  scripts: Scripts.t,
  solved: Run.t(solved),
}
and solved = {
  solution: Solution.t,
  fetched: Run.t(fetched),
}
and fetched = {
  installation: Installation.t,
  sandbox: BuildSandbox.t,
  configured: Run.t(configured),
}
and configured = {
  planForDev: BuildSandbox.Plan.t,
  root: BuildSandbox.Task.t,
};

type t = project;

module TermPp = {
  let ppOption = (name, pp, fmt, option) =>
    switch (option) {
    | None => Fmt.string(fmt, "")
    | Some(v) => Fmt.pf(fmt, "%s %a \\@;", name, pp, v)
    };

  let ppFlag = (flag, fmt, enabled) =>
    if (enabled) {
      Fmt.pf(fmt, "%s \\@;", flag);
    } else {
      Fmt.string(fmt, "");
    };

  let ppEnvSpec = (fmt, envspec) => {
    let {
      EnvSpec.augmentDeps,
      buildIsInProgress,
      includeCurrentEnv,
      includeBuildEnv,
      includeEsyIntrospectionEnv,
      includeNpmBin,
    } = envspec;
    Fmt.pf(
      fmt,
      "%a%a%a%a%a%a",
      ppOption("--envspec", Fmt.quote(~mark="'", Solution.DepSpec.pp)),
      augmentDeps,
      ppFlag("--build-context"),
      buildIsInProgress,
      ppFlag("--include-current-env"),
      includeCurrentEnv,
      ppFlag("--include-npm-bin"),
      includeNpmBin,
      ppFlag("--include-esy-introspection-env"),
      includeEsyIntrospectionEnv,
      ppFlag("--include-build-env"),
      includeBuildEnv,
    );
  };
};

let makeCachePath = (prefix, projcfg: ProjectConfig.t) => {
  let json = ProjectConfig.to_yojson(projcfg);
  let hash = Yojson.Safe.to_string(json) |> Digest.string |> Digest.to_hex;

  Path.(SandboxSpec.cachePath(projcfg.spec) / (prefix ++ "-" ++ hash));
};

let solved = proj => Lwt.return(proj.solved);

let fetched = proj =>
  Lwt.return(
    {
      open Result.Syntax;
      let%bind solved = proj.solved;
      solved.fetched;
    },
  );

let configured = proj =>
  Lwt.return(
    {
      open Result.Syntax;
      let%bind solved = proj.solved;
      let%bind fetched = solved.fetched;
      fetched.configured;
    },
  );

let makeProject = (makeSolved, projcfg: ProjectConfig.t) => {
  open RunAsync.Syntax;
  let workflow = Workflow.default;
  let%bind files = {
    let paths = SandboxSpec.manifestPaths(projcfg.spec);
    RunAsync.List.mapAndJoin(~f=FileInfo.ofPath, paths);
  };

  let files = ref(files);

  let%bind esySolveCmd =
    switch (projcfg.solveCudfCommand) {
    | Some(cmd) => return(cmd)
    | None =>
      let cmd = EsyRuntime.resolve("esy-solve-cudf/esySolveCudfCommand.exe");
      return(Cmd.(v(p(cmd))));
    };

  let%bind solveCfg =
    EsySolve.Config.make(
      ~esySolveCmd,
      ~skipRepositoryUpdate=projcfg.skipRepositoryUpdate,
      ~cachePath=?projcfg.cachePath,
      ~cacheTarballsPath=?projcfg.cacheTarballsPath,
      ~npmRegistry=?projcfg.npmRegistry,
      ~opamRepository=?projcfg.opamRepository,
      ~esyOpamOverride=?projcfg.esyOpamOverride,
      ~solveTimeout=?projcfg.solveTimeout,
      (),
    );

  let installCfg = solveCfg.EsySolve.Config.installCfg;
  let%bind solveSandbox = EsySolve.Sandbox.make(~cfg=solveCfg, projcfg.spec);
  let installSandbox = EsyInstall.Sandbox.make(installCfg, projcfg.spec);

  let%bind buildCfg = {
    let storePath =
      switch (projcfg.prefixPath) {
      | None => EsyBuildPackage.Config.StorePathDefault
      | Some(prefixPath) =>
        EsyBuildPackage.Config.StorePathOfPrefix(prefixPath)
      };

    RunAsync.ofBosError(
      EsyBuildPackage.Config.make(
        ~storePath,
        ~localStorePath=EsyInstall.SandboxSpec.storePath(projcfg.spec),
        ~buildPath=EsyInstall.SandboxSpec.buildPath(projcfg.spec),
        ~projectPath=projcfg.spec.path,
        (),
      ),
    );
  };

  let%bind scripts = Scripts.ofSandbox(projcfg.ProjectConfig.spec);
  let%lwt solved =
    makeSolved(
      projcfg,
      workflow,
      buildCfg,
      solveSandbox,
      installSandbox,
      files,
    );
  return((
    {
      projcfg,
      buildCfg,
      spec: projcfg.spec,
      scripts,
      solved,
      workflow,
      solveSandbox,
      installSandbox,
    },
    files^,
  ));
};

let makeSolved =
    (
      makeFetched,
      projcfg: ProjectConfig.t,
      workflow,
      buildCfg,
      solver,
      installer,
      files,
    ) => {
  open RunAsync.Syntax;
  let path = SandboxSpec.solutionLockPath(projcfg.spec);
  let%bind info = FileInfo.ofPath(Path.(path / "index.json"));
  files := [info, ...files^];
  let%bind digest =
    EsySolve.Sandbox.digest(Workflow.default.solvespec, solver);

  switch%bind (SolutionLock.ofPath(~digest, installer, path)) {
  | Some(solution) =>
    let%lwt fetched =
      makeFetched(
        projcfg,
        workflow,
        buildCfg,
        solver,
        installer,
        solution,
        files,
      );
    return({solution, fetched});
  | None => errorf("project is missing a lock, run `esy install`")
  };
};

module OfPackageJson = {
  [@deriving of_yojson({strict: false})]
  type esy = {
    [@default BuildEnv.empty]
    sandboxEnv: BuildEnv.t,
  };

  [@deriving of_yojson({strict: false})]
  type t = {
    [@default {sandboxEnv: BuildEnv.empty}]
    esy,
  };
};

let readSandboxEnv = spec =>
  RunAsync.Syntax.(
    switch (spec.EsyInstall.SandboxSpec.manifest) {
    | [@implicit_arity] EsyInstall.SandboxSpec.Manifest(Esy, filename) =>
      let%bind json = Fs.readJsonFile(Path.(spec.path / filename));
      let%bind pkgJson =
        RunAsync.ofRun(Json.parseJsonWith(OfPackageJson.of_yojson, json));
      return(pkgJson.OfPackageJson.esy.sandboxEnv);

    | [@implicit_arity] EsyInstall.SandboxSpec.Manifest(Opam, _)
    | EsyInstall.SandboxSpec.ManifestAggregate(_) => return(BuildEnv.empty)
    }
  );

let makeFetched =
    (
      makeConfigured,
      projcfg: ProjectConfig.t,
      workflow,
      buildCfg,
      _solver,
      installer,
      solution,
      files,
    ) => {
  open RunAsync.Syntax;
  let path = EsyInstall.SandboxSpec.installationPath(projcfg.spec);
  let%bind info = FileInfo.ofPath(path);
  files := [info, ...files^];
  switch%bind (Installation.ofPath(path)) {
  | None => errorf("project is not installed, run `esy install`")
  | Some(installation) =>
    let isActual = {
      let nodes = Solution.nodes(solution);
      let checkPackageIsInInstallation = (isActual, pkg) =>
        if (!isActual) {
          isActual;
        } else {
          let check = Installation.mem(pkg.Package.id, installation);
          if (!check) {
            Logs.debug(m =>
              m("missing from installation %a", PackageId.pp, pkg.Package.id)
            );
          };
          check;
        };

      List.fold_left(~f=checkPackageIsInInstallation, ~init=true, nodes);
    };

    if (isActual) {
      let%bind sandbox = {
        let%bind sandboxEnv = readSandboxEnv(projcfg.spec);
        let%bind (sandbox, filesUsedForPlan) =
          BuildSandbox.make(
            ~sandboxEnv,
            buildCfg,
            projcfg.spec,
            installer.EsyInstall.Sandbox.cfg,
            solution,
            installation,
          );

        let%bind filesUsedForPlan = FileInfo.ofPathSet(filesUsedForPlan);
        files := files^ @ filesUsedForPlan;
        return(sandbox);
      };

      let%lwt configured =
        makeConfigured(
          projcfg,
          workflow,
          solution,
          installation,
          sandbox,
          files,
        );
      return({installation, sandbox, configured});
    } else {
      errorf(
        "project requires to update its installation, run `esy install`",
      );
    };
  };
};

let makeConfigured =
    (_projcfg, workflow, solution, _installation, sandbox, _files) => {
  open RunAsync.Syntax;

  let%bind (root, planForDev) =
    RunAsync.ofRun(
      {
        open Run.Syntax;
        let%bind plan =
          BuildSandbox.makePlan(
            workflow.Workflow.buildspec,
            BuildDev,
            sandbox,
          );

        let pkg = EsyInstall.Solution.root(solution);
        let root =
          switch (BuildSandbox.Plan.get(plan, pkg.Package.id)) {
          | None => failwith("missing build for the root package")
          | Some(task) => task
          };

        return((root, plan));
      },
    );

  return({planForDev, root});
};

let plan = (mode, proj) =>
  RunAsync.Syntax.(
    switch (mode) {
    | BuildSpec.Build =>
      let%bind fetched = fetched(proj);
      Lwt.return(
        BuildSandbox.makePlan(
          Workflow.default.buildspec,
          Build,
          fetched.sandbox,
        ),
      );
    | BuildSpec.BuildDev =>
      let%bind configured = configured(proj);
      return(configured.planForDev);
    }
  );

let make = projcfg =>
  makeProject(makeSolved(makeFetched(makeConfigured)), projcfg);

let writeAuxCache = proj => {
  open RunAsync.Syntax;
  let info = {
    let%bind solved = solved(proj);
    let%bind fetched = fetched(proj);
    return((solved, fetched));
  };

  switch%lwt (info) {
  | Error(_) => return()
  | [@implicit_arity] Ok(solved, fetched) =>
    let sandboxBin = SandboxSpec.binPath(proj.projcfg.spec);
    let sandboxBinLegacyPath =
      Path.(
        proj.projcfg.spec.path
        / "node_modules"
        / ".cache"
        / "_esy"
        / "build"
        / "bin"
      );
    let root = Solution.root(solved.solution);
    let%bind () = Fs.createDir(sandboxBin);
    let%bind commandEnv =
      RunAsync.ofRun(
        {
          open Run.Syntax;
          let header = "# Command environment";
          let%bind commandEnv =
            BuildSandbox.env(
              proj.workflow.commandenvspec,
              proj.workflow.buildspec,
              BuildDev,
              fetched.sandbox,
              root.Package.id,
            );

          let commandEnv =
            Scope.SandboxEnvironment.Bindings.render(
              proj.buildCfg,
              commandEnv,
            );
          Environment.renderToShellSource(~header, commandEnv);
        },
      );
    let commandExec = "#!/bin/bash\n" ++ commandEnv ++ "\nexec \"$@\"";

    let%bind () =
      RunAsync.List.waitAll([
        Fs.writeFile(~data=commandEnv, Path.(sandboxBin / "command-env")),
        Fs.writeFile(
          ~perm=0o755,
          ~data=commandExec,
          Path.(sandboxBin / "command-exec"),
        ),
      ]);

    let%bind () = Fs.createDir(sandboxBinLegacyPath);
    RunAsync.List.waitAll([
      Fs.writeFile(
        ~data=commandEnv,
        Path.(sandboxBinLegacyPath / "command-env"),
      ),
      Fs.writeFile(
        ~perm=0o755,
        ~data=commandExec,
        Path.(sandboxBinLegacyPath / "command-exec"),
      ),
    ]);
  };
};

let resolvePackage = (~name, proj: project) => {
  open RunAsync.Syntax;
  let%bind solved = solved(proj);
  let%bind fetched = fetched(proj);
  let%bind configured = configured(proj);

  switch (Solution.findByName(name, solved.solution)) {
  | None =>
    errorf("package %s is not installed as a part of the project", name)
  | Some(_) =>
    let%bind (task, sandbox) =
      RunAsync.ofRun(
        {
          open Run.Syntax;
          let task = {
            open Option.Syntax;
            let%bind task =
              BuildSandbox.Plan.getByName(configured.planForDev, name);
            return(task);
          };

          return((task, fetched.sandbox));
        },
      );
    switch (task) {
    | None => errorf("package %s isn't built yet, run 'esy build'", name)
    | Some(task) =>
      if%bind (BuildSandbox.isBuilt(sandbox, task)) {
        return(BuildSandbox.Task.installPath(proj.buildCfg, task));
      } else {
        errorf("package %s isn't built yet, run 'esy build'", name);
      }
    };
  };
};

let ocamlfind = resolvePackage(~name="@opam/ocamlfind");
let ocaml = resolvePackage(~name="ocaml");

module OfTerm = {
  let checkStaleness = files => {
    open RunAsync.Syntax;
    let files = files;
    let%bind checks =
      RunAsync.List.joinAll(
        {
          let f = prev => {
            let%bind next = FileInfo.ofPath(prev.FileInfo.path);
            let changed = FileInfo.compare(prev, next) != 0;
            let%lwt () =
              Logs_lwt.debug(m =>
                m(
                  "checkStaleness %a: %b",
                  Path.pp,
                  prev.FileInfo.path,
                  changed,
                )
              );
            return(changed);
          };

          List.map(~f, files);
        },
      );
    return(List.exists(~f=x => x, checks));
  };

  let read' = (projcfg, ()) => {
    open RunAsync.Syntax;
    let cachePath = makeCachePath("project", projcfg);
    let f = ic =>
      try%lwt (
        {
          let%lwt (v, files) = (
            Lwt_io.read_value(ic): Lwt.t((project, list(FileInfo.t)))
          );
          let v = {...v, projcfg};
          if%bind (checkStaleness(files)) {
            return(None);
          } else {
            return(Some(v));
          };
        }
      ) {
      | Failure(_) => return(None)
      };

    try%lwt (Lwt_io.with_file(~mode=Lwt_io.Input, Path.show(cachePath), f)) {
    | Unix.Unix_error(_) => return(None)
    };
  };

  let read = projcfg =>
    Perf.measureLwt(~label="reading project cache", read'(projcfg));

  let write' = (projcfg, v, files, ()) => {
    open RunAsync.Syntax;
    let cachePath = makeCachePath("project", projcfg);
    let%bind () = {
      let f = oc => {
        let%lwt () =
          Lwt_io.write_value(~flags=Marshal.[Closures], oc, (v, files));
        let%lwt () = Lwt_io.flush(oc);
        return();
      };

      let%bind () = Fs.createDir(Path.parent(cachePath));
      Lwt_io.with_file(~mode=Lwt_io.Output, Path.show(cachePath), f);
    };

    let%bind () = writeAuxCache(v);
    return();
  };

  let write = (projcfg, v, files) =>
    Perf.measureLwt(
      ~label="writing project cache",
      write'(projcfg, v, files),
    );

  let promiseTerm = {
    let parse = projcfg => {
      open RunAsync.Syntax;
      let%bind projcfg = projcfg;
      switch%bind (read(projcfg)) {
      | Some(proj) => return(proj)
      | None =>
        let%bind (proj, files) = make(projcfg);
        let%bind () = write(projcfg, proj, files);
        return(proj);
      };
    };

    Cmdliner.Term.(const(parse) $ ProjectConfig.promiseTerm);
  };

  let term =
    Cmdliner.Term.(ret(const(Cli.runAsyncToCmdlinerRet) $ promiseTerm));
};

include OfTerm;

let withPackage = (proj, pkgArg: PkgArg.t, f) => {
  open RunAsync.Syntax;
  let%bind solved = solved(proj);
  let solution = solved.solution;
  let runWith = pkg =>
    switch (pkg) {
    | Some(pkg) =>
      let%lwt () =
        Logs_lwt.debug(m =>
          m("PkgArg %a resolves to %a", PkgArg.pp, pkgArg, Package.pp, pkg)
        );
      f(pkg);
    | None => errorf("no package found: %a", PkgArg.pp, pkgArg)
    };

  let pkg =
    switch (pkgArg) {
    | ByPkgSpec(Root) => Some(Solution.root(solution))
    | ByPkgSpec(ByName(name)) => Solution.findByName(name, solution)
    | ByPkgSpec([@implicit_arity] ByNameVersion(name, version)) =>
      Solution.findByNameVersion(name, version, solution)
    | ByPkgSpec(ById(id)) => Solution.get(solution, id)
    | ByPath(path) =>
      let root = proj.installSandbox.spec.path;
      let path = Path.(EsyRuntime.currentWorkingDir /\/ path);
      let path = DistPath.ofPath(Path.tryRelativize(~root, path));
      Solution.findByPath(path, solution);
    };

  runWith(pkg);
};

let buildDependencies =
    (~skipStalenessCheck=false, ~buildLinked, proj: project, plan, pkg) => {
  open RunAsync.Syntax;
  let%bind fetched = fetched(proj);
  let%bind solved = solved(proj);
  let () =
    Logs.info(m =>
      m(
        "running:@[<v>@;%s build-dependencies \\@;%a%a@]",
        proj.projcfg.ProjectConfig.mainprg,
        TermPp.(ppFlag("--all")),
        buildLinked,
        PackageId.pp,
        pkg.Package.id,
      )
    );

  switch (BuildSandbox.Plan.get(plan, pkg.id)) {
  | None => RunAsync.return()
  | Some(task) =>
    let depspec = Scope.depspec(task.scope);
    let dependencies =
      Solution.dependenciesByDepSpec(solved.solution, depspec, task.pkg);
    BuildSandbox.build(
      ~skipStalenessCheck,
      ~concurrency=EsyRuntime.concurrency,
      ~buildLinked,
      fetched.sandbox,
      plan,
      List.map(~f=pkg => pkg.Package.id, dependencies),
    );
  };
};

let buildPackage = (~quiet, ~buildOnly, projcfg, sandbox, plan, pkg) => {
  let () =
    Logs.info(m =>
      m(
        "running:@[<v>@;%s build-package \\@;%a@]",
        projcfg.ProjectConfig.mainprg,
        PackageId.pp,
        pkg.Package.id,
      )
    );

  BuildSandbox.buildOnly(
    ~force=true,
    ~quiet,
    ~buildOnly,
    sandbox,
    plan,
    pkg.id,
  );
};

let printEnv =
    (~name="Environment", proj: project, envspec, mode, asJson, pkgarg, ()) => {
  open RunAsync.Syntax;

  let%bind _solved = solved(proj);
  let%bind fetched = fetched(proj);

  let f = (pkg: Package.t) => {
    let () =
      Logs.info(m =>
        m(
          "running:@[<v>@;%s print-env \\@;%a%a@]",
          proj.projcfg.ProjectConfig.mainprg,
          TermPp.ppEnvSpec,
          envspec,
          PackageId.pp,
          pkg.Package.id,
        )
      );

    let%bind source =
      RunAsync.ofRun(
        {
          open Run.Syntax;
          let%bind (env, scope) =
            BuildSandbox.configure(
              envspec,
              Workflow.default.buildspec,
              mode,
              fetched.sandbox,
              pkg.id,
            );

          let env =
            Scope.SandboxEnvironment.Bindings.render(proj.buildCfg, env);
          if (asJson) {
            let%bind env = Run.ofStringError(Environment.Bindings.eval(env));
            Ok(env |> Environment.to_yojson |> Yojson.Safe.pretty_to_string);
          } else {
            let mode = Scope.mode(scope);
            let depspec = Scope.depspec(scope);
            let header =
              Format.asprintf(
                {|# %s
# package:            %a
# depspec:            %a
# mode:               %a
# envspec:            %a
# buildIsInProgress:  %b
# includeBuildEnv:    %b
# includeCurrentEnv:  %b
# includeNpmBin:      %b
|},
                name,
                Package.pp,
                pkg,
                Solution.DepSpec.pp,
                depspec,
                BuildSpec.pp_mode,
                mode,
                Fmt.option(Solution.DepSpec.pp),
                envspec.EnvSpec.augmentDeps,
                envspec.buildIsInProgress,
                envspec.includeBuildEnv,
                envspec.includeCurrentEnv,
                envspec.includeNpmBin,
              );

            Environment.renderToShellSource(~header, env);
          };
        },
      );

    let%lwt () = Lwt_io.print(source);
    return();
  };

  withPackage(proj, pkgarg, f);
};

let execCommand =
    (
      ~checkIfDependenciesAreBuilt,
      ~buildLinked,
      proj: project,
      envspec,
      mode,
      pkg: Package.t,
      cmd,
    ) => {
  open RunAsync.Syntax;

  let%bind fetched = fetched(proj);

  let%bind () =
    if (checkIfDependenciesAreBuilt) {
      let%bind plan = plan(mode, proj);
      buildDependencies(~buildLinked, proj, plan, pkg);
    } else {
      return();
    };

  let () =
    Logs.info(m =>
      m(
        "running:@[<v>@;%s exec-command \\@;%a%a \\@;-- %a@]",
        proj.projcfg.ProjectConfig.mainprg,
        TermPp.ppEnvSpec,
        envspec,
        PackageId.pp,
        pkg.Package.id,
        Cmd.pp,
        cmd,
      )
    );

  let%bind status =
    BuildSandbox.exec(
      envspec,
      Workflow.default.buildspec,
      mode,
      fetched.sandbox,
      pkg.id,
      cmd,
    );

  switch (status) {
  | Unix.WEXITED(n)
  | Unix.WSTOPPED(n)
  | Unix.WSIGNALED(n) => exit(n)
  };
};