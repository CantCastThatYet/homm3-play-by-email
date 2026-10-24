FROM mcr.microsoft.com/dotnet/sdk:10.0 AS build
WORKDIR /source
COPY global.json Directory.Build.props ./
COPY src/HeroesLobby.Contracts/ src/HeroesLobby.Contracts/
COPY src/HeroesLobby.Server/ src/HeroesLobby.Server/
RUN dotnet publish src/HeroesLobby.Server -c Release -o /app

FROM mcr.microsoft.com/dotnet/aspnet:10.0
WORKDIR /app
COPY --from=build /app .
ENV URLS=http://0.0.0.0:8080
ENV Lobby__Database=/data/lobby.db
RUN mkdir /data && chown app:app /data
USER app
EXPOSE 8080
VOLUME /data
ENTRYPOINT ["dotnet", "HeroesLobby.Server.dll"]
